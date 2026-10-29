#include "RmlUi_Renderer_SDL_GPU.h"
#include "RmlUi_SDL_GPU/ShadersCompiledSPV.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Types.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cstring>
#include <type_traits>

using namespace Rml;

static constexpr int MaxNumStops = 16;           // must match MAX_NUM_STOPS in shader_frag_gradient.frag
static constexpr int BlurSize = 7;               // the taps in shader_vert_blur.vert and shader_frag_blur.frag
static constexpr int BlurNumWeights = (BlurSize + 1) / 2;
static constexpr size_t MaxNumRenderTargets = 4; // frame sizes whose textures are kept, e.g. for several windows
static constexpr SDL_GPUTextureFormat LayerFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

using ProgramId = RenderInterface_SDL_GPU::ProgramId;
using TargetKind = RenderInterface_SDL_GPU::TargetKind;
using BlendState = RenderInterface_SDL_GPU::BlendState;
using StencilState = RenderInterface_SDL_GPU::StencilState;

// File-local types: the OpenGL 3 renderer, which may be linked into the same program, has types of the same names.
namespace {

enum class VertexShaderId { Main, Passthrough, Blur, Count };
enum class FragmentShaderId { Color, Texture, Gradient, Passthrough, ColorMatrix, BlendMask, Blur, DropShadow, Count };

enum ShaderFormat {
	ShaderFormatSPIRV,
	ShaderFormatMSL,
	ShaderFormatDXIL,
	ShaderFormatCount,
};

struct ShaderDefinition {
	Span<const byte> data[ShaderFormatCount];
	Uint32 uniforms;
	Uint32 samplers;
};

#define X(name)            \
	Span<const byte>       \
	{                      \
		name, sizeof(name) \
	}
#define SHADER(name) {X(name##_spirv), X(name##_msl), X(name##_dxil)}

// clang-format off
static const ShaderDefinition vertex_shader_definitions[] = {
	{SHADER(shader_vert),             2, 0}, // Main
	{SHADER(shader_vert_passthrough), 1, 0}, // Passthrough
	{SHADER(shader_vert_blur),        1, 0}, // Blur
};
static const ShaderDefinition fragment_shader_definitions[] = {
	{SHADER(shader_frag_color),        0, 0}, // Color
	{SHADER(shader_frag_texture),      0, 1}, // Texture
	{SHADER(shader_frag_gradient),     1, 0}, // Gradient
	{SHADER(shader_frag_passthrough),  0, 1}, // Passthrough
	{SHADER(shader_frag_color_matrix), 1, 1}, // ColorMatrix
	{SHADER(shader_frag_blend_mask),   0, 2}, // BlendMask
	{SHADER(shader_frag_blur),         1, 1}, // Blur
	{SHADER(shader_frag_drop_shadow),  1, 1}, // DropShadow
};
struct ProgramDefinition {
	VertexShaderId vertex;
	FragmentShaderId fragment;
};
static const ProgramDefinition program_definitions[] = {
	{VertexShaderId::Main,        FragmentShaderId::Color},       // Color
	{VertexShaderId::Main,        FragmentShaderId::Texture},     // Texture
	{VertexShaderId::Main,        FragmentShaderId::Gradient},    // Gradient
	{VertexShaderId::Passthrough, FragmentShaderId::Passthrough}, // Passthrough
	{VertexShaderId::Passthrough, FragmentShaderId::ColorMatrix}, // ColorMatrix
	{VertexShaderId::Passthrough, FragmentShaderId::BlendMask},   // BlendMask
	{VertexShaderId::Blur,        FragmentShaderId::Blur},        // Blur
	{VertexShaderId::Passthrough, FragmentShaderId::DropShadow},  // DropShadow
	{VertexShaderId::Passthrough, FragmentShaderId::Color},       // Fill: the fullscreen quad's color, transparent
};
// clang-format on

#undef SHADER
#undef X

static_assert(sizeof(vertex_shader_definitions) / sizeof(ShaderDefinition) == size_t(VertexShaderId::Count), "");
static_assert(sizeof(fragment_shader_definitions) / sizeof(ShaderDefinition) == size_t(FragmentShaderId::Count), "");
static_assert(sizeof(program_definitions) / sizeof(ProgramDefinition) == size_t(ProgramId::Count), "");

// Uniform blocks, laid out as the shaders' cbuffers.
struct TexCoordTransformUniforms {
	Vector2f scale;
	Vector2f offset;
};
struct GradientUniforms {
	int function;
	int num_stops;
	int padding[2];
	Vector2f p;
	Vector2f v;
	float stop_positions[MaxNumStops];
	Colourf stop_colors[MaxNumStops];
};
static_assert(sizeof(GradientUniforms) == 16 * (2 + MaxNumStops / 4 + MaxNumStops), "");
struct BlurUniforms {
	float weights[BlurNumWeights];
	Vector2f tex_coord_min;
	Vector2f tex_coord_max;
};
static_assert(sizeof(BlurUniforms) == 32, "");
struct DropShadowUniforms {
	Colourf color;
	Vector2f tex_coord_min;
	Vector2f tex_coord_max;
};
static_assert(sizeof(DropShadowUniforms) == 32, "");

enum class ShaderGradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic }; // as in the shader

enum class CompiledShaderType { Invalid = 0, Gradient };
struct CompiledShader {
	CompiledShaderType type;
	GradientUniforms gradient;
};

enum class FilterType { Invalid = 0, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };
struct CompiledFilter {
	FilterType type;

	// Passthrough
	float blend_factor;

	// Blur
	float sigma;

	// Drop shadow
	Vector2f offset;
	ColourbPremultiplied color;

	// ColorMatrix
	Matrix4f color_matrix;
};

} // namespace

static SDL_GPUShader* CreateShader(SDL_GPUDevice* device, const ShaderDefinition& shader, SDL_GPUShaderStage stage)
{
	SDL_GPUShaderFormat sdl_shader_format = SDL_GetGPUShaderFormats(device);
	ShaderFormat format = ShaderFormatCount;
	const char* entrypoint = nullptr;
	if (sdl_shader_format & SDL_GPU_SHADERFORMAT_SPIRV)
	{
		sdl_shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
		format = ShaderFormatSPIRV;
		entrypoint = "main";
	}
	else if (sdl_shader_format & SDL_GPU_SHADERFORMAT_DXIL)
	{
		sdl_shader_format = SDL_GPU_SHADERFORMAT_DXIL;
		format = ShaderFormatDXIL;
		entrypoint = "main";
	}
	else if (sdl_shader_format & SDL_GPU_SHADERFORMAT_MSL)
	{
		sdl_shader_format = SDL_GPU_SHADERFORMAT_MSL;
		format = ShaderFormatMSL;
		entrypoint = "main0";
	}
	else
	{
		RMLUI_ERRORMSG("Invalid shader format");
		return nullptr;
	}
	SDL_GPUShaderCreateInfo info{};
	info.code = static_cast<const Uint8*>(shader.data[format].data());
	info.code_size = shader.data[format].size();
	info.entrypoint = entrypoint;
	info.format = sdl_shader_format;
	info.stage = stage;
	info.num_samplers = shader.samplers;
	info.num_uniform_buffers = shader.uniforms;
	SDL_GPUShader* sdl_shader = SDL_CreateGPUShader(device, &info);
	if (!sdl_shader)
	{
		Log::Message(Log::LT_ERROR, "Failed to create shader: %s", SDL_GetError());
		RMLUI_ERROR;
	}
	return sdl_shader;
}

static SDL_GPUSampleCount ToSampleCount(int samples)
{
	if (samples >= 8)
		return SDL_GPU_SAMPLECOUNT_8;
	if (samples >= 4)
		return SDL_GPU_SAMPLECOUNT_4;
	if (samples >= 2)
		return SDL_GPU_SAMPLECOUNT_2;
	return SDL_GPU_SAMPLECOUNT_1;
}

static SDL_GPUTexture* CreateTexture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int width, int height,
	SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1)
{
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = format;
	info.usage = usage;
	info.width = Uint32(width);
	info.height = Uint32(height);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = samples;
	SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
	if (!texture)
		Log::Message(Log::LT_ERROR, "Failed to create texture: %s", SDL_GetError());
	return texture;
}

static Colourf ConvertToColorf(ColourbPremultiplied c0)
{
	Colourf result;
	for (int i = 0; i < 4; i++)
		result[i] = (1.f / 255.f) * float(c0[i]);
	return result;
}

static void SigmaToParameters(const float desired_sigma, int& out_pass_level, float& out_sigma)
{
	constexpr int max_num_passes = 10;
	static_assert(max_num_passes < 31, "");
	constexpr float max_single_pass_sigma = 3.0f;
	out_pass_level = Math::Clamp(Math::Log2(int(desired_sigma * (2.f / max_single_pass_sigma))), 0, max_num_passes);
	out_sigma = Math::Clamp(desired_sigma / float(1 << out_pass_level), 0.0f, max_single_pass_sigma);
}

static void SetBlurWeights(float (&weights)[BlurNumWeights], float sigma)
{
	float normalization = 0.0f;
	for (int i = 0; i < BlurNumWeights; i++)
	{
		if (Math::Absolute(sigma) < 0.1f)
			weights[i] = float(i == 0);
		else
			weights[i] = Math::Exp(-float(i * i) / (2.0f * sigma * sigma)) / (Math::SquareRoot(2.f * Math::RMLUI_PI) * sigma);

		normalization += (i == 0 ? 1.f : 2.0f) * weights[i];
	}
	for (int i = 0; i < BlurNumWeights; i++)
		weights[i] /= normalization;
}

// Texture coordinate limits that keep bilinear lookups inside the rectangle: at the centers of its edge texels.
static void SetTexCoordLimits(Vector2f& out_min, Vector2f& out_max, Rectanglei rectangle, Vector2i texture_size)
{
	out_min = (Vector2f(rectangle.p0) + Vector2f(0.5f)) / Vector2f(texture_size);
	out_max = (Vector2f(rectangle.p1) - Vector2f(0.5f)) / Vector2f(texture_size);
}

RenderInterface_SDL_GPU::RenderInterface_SDL_GPU(SDL_GPUDevice* device, SDL_Window* window, int msaa_samples) : device(device), window(window)
{
	output_format = SDL_GetGPUSwapchainTextureFormat(device, window);

	depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
	if (!SDL_GPUTextureSupportsFormat(device, depth_stencil_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
		depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

	// Like OpenGL, which gives at least as many samples as asked for: the smallest count the device supports from the
	// requested one up (Mesa's lavapipe, e.g., has 4 but not 2), else the largest below it.
	const SDL_GPUSampleCount requested = ToSampleCount(msaa_samples);
	auto Supported = [&](SDL_GPUSampleCount count) {
		return count == SDL_GPU_SAMPLECOUNT_1 ||
			(SDL_GPUTextureSupportsSampleCount(device, LayerFormat, count) && SDL_GPUTextureSupportsSampleCount(device, depth_stencil_format, count));
	};
	sample_count = SDL_GPU_SAMPLECOUNT_1;
	for (int count = int(requested); count <= int(SDL_GPU_SAMPLECOUNT_8); count++)
	{
		if (Supported(SDL_GPUSampleCount(count)))
		{
			sample_count = SDL_GPUSampleCount(count);
			break;
		}
	}
	for (int count = int(requested) - 1; count > int(sample_count) && sample_count < requested; count--)
		if (Supported(SDL_GPUSampleCount(count)))
			sample_count = SDL_GPUSampleCount(count);

	for (size_t i = 0; i < size_t(VertexShaderId::Count); i++)
		vertex_shaders[i] = CreateShader(device, vertex_shader_definitions[i], SDL_GPU_SHADERSTAGE_VERTEX);
	for (size_t i = 0; i < size_t(FragmentShaderId::Count); i++)
		fragment_shaders[i] = CreateShader(device, fragment_shader_definitions[i], SDL_GPU_SHADERSTAGE_FRAGMENT);

	SDL_GPUSamplerCreateInfo info{};
	info.min_filter = SDL_GPU_FILTER_LINEAR;
	info.mag_filter = SDL_GPU_FILTER_LINEAR;
	info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	linear_sampler = SDL_CreateGPUSampler(device, &info);
	// Layers and postprocess textures: lookups outside them don't wrap around.
	info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	clamp_sampler = SDL_CreateGPUSampler(device, &info);
	if (!linear_sampler || !clamp_sampler)
		Log::Message(Log::LT_ERROR, "Failed to create sampler: %s", SDL_GetError());

	// A quad covering the target in clip space, with texture coordinates from its top left, and a transparent color
	// for the Fill program.
	const ColourbPremultiplied transparent(0, 0, 0, 0);
	const Vertex vertices[4] = {
		{{-1.f, 1.f}, transparent, {0.f, 0.f}},
		{{1.f, 1.f}, transparent, {1.f, 0.f}},
		{{1.f, -1.f}, transparent, {1.f, 1.f}},
		{{-1.f, -1.f}, transparent, {0.f, 1.f}},
	};
	const int indices[6] = {0, 1, 2, 0, 2, 3};
	fullscreen_quad = reinterpret_cast<GeometryView*>(CompileGeometry({vertices, 4}, {indices, 6}));
}

int RenderInterface_SDL_GPU::GetMsaaSamples() const
{
	switch (sample_count)
	{
	case SDL_GPU_SAMPLECOUNT_1: return 1;
	case SDL_GPU_SAMPLECOUNT_2: return 2;
	case SDL_GPU_SAMPLECOUNT_4: return 4;
	case SDL_GPU_SAMPLECOUNT_8: return 8;
	}
	return 1;
}

void RenderInterface_SDL_GPU::Shutdown()
{
	ExecuteCommands(); // releases that RmlUi asked for since the last frame

	if (fullscreen_quad)
	{
		fullscreen_quad->vertex_buffer->in_use = false;
		fullscreen_quad->index_buffer->in_use = false;
		delete fullscreen_quad;
		fullscreen_quad = nullptr;
	}
	for (Rml::UniquePtr<RenderTargets>& render_target : render_targets)
		ReleaseRenderTargets(*render_target);
	render_targets.clear();
	for (Rml::UniquePtr<Buffer>& buffer : buffers)
	{
		SDL_ReleaseGPUTransferBuffer(device, buffer->transfer_buffer);
		SDL_ReleaseGPUBuffer(device, buffer->buffer);
	}
	buffers.clear();
	SDL_ReleaseGPUSampler(device, linear_sampler);
	SDL_ReleaseGPUSampler(device, clamp_sampler);
	for (auto& by_target : pipelines)
		for (auto& by_blend : by_target)
			for (auto& by_stencil : by_blend)
				for (SDL_GPUGraphicsPipeline*& pipeline : by_stencil)
				{
					if (pipeline)
						SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
					pipeline = nullptr;
				}
	for (SDL_GPUShader*& shader : vertex_shaders)
	{
		SDL_ReleaseGPUShader(device, shader);
		shader = nullptr;
	}
	for (SDL_GPUShader*& shader : fragment_shaders)
	{
		SDL_ReleaseGPUShader(device, shader);
		shader = nullptr;
	}
}

SDL_GPUGraphicsPipeline* RenderInterface_SDL_GPU::GetPipeline(ProgramId program, TargetKind kind, BlendState blend, StencilState stencil)
{
	SDL_GPUGraphicsPipeline*& pipeline = pipelines[size_t(program)][size_t(kind)][size_t(blend)][size_t(stencil)];
	if (pipeline)
		return pipeline;

	SDL_GPUColorTargetDescription target{};
	target.format = (kind == TargetKind::Output ? output_format : LayerFormat);
	target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	switch (blend)
	{
	case BlendState::None: break;
	case BlendState::Premultiplied:
		target.blend_state.enable_blend = true;
		target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case BlendState::Constant:
		target.blend_state.enable_blend = true;
		target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		break;
	case BlendState::Count: break;
	}
	const bool writes_stencil = (stencil == StencilState::WriteReplace || stencil == StencilState::WriteIncrement);
	if (writes_stencil)
	{
		// Only the clip mask: no colors.
		target.blend_state.enable_color_write_mask = true;
		target.blend_state.color_write_mask = 0;
	}

	SDL_GPUVertexAttribute attributes[3]{};
	attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attributes[0].location = 0;
	attributes[0].offset = offsetof(Vertex, position);
	attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
	attributes[1].location = 1;
	attributes[1].offset = offsetof(Vertex, colour);
	attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attributes[2].location = 2;
	attributes[2].offset = offsetof(Vertex, tex_coord);

	SDL_GPUVertexBufferDescription buffer{};
	buffer.pitch = sizeof(Vertex);

	SDL_GPUGraphicsPipelineCreateInfo info{};
	const ProgramDefinition& definition = program_definitions[size_t(program)];
	info.vertex_shader = vertex_shaders[size_t(definition.vertex)];
	info.fragment_shader = fragment_shaders[size_t(definition.fragment)];
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &target;
	info.vertex_input_state.num_vertex_attributes = 3;
	info.vertex_input_state.num_vertex_buffers = 1;
	info.vertex_input_state.vertex_attributes = attributes;
	info.vertex_input_state.vertex_buffer_descriptions = &buffer;

	if (kind == TargetKind::Layer)
	{
		info.multisample_state.sample_count = sample_count;
		info.target_info.has_depth_stencil_target = true;
		info.target_info.depth_stencil_format = depth_stencil_format;
		if (stencil != StencilState::Disabled)
		{
			SDL_GPUStencilOpState op{};
			op.fail_op = SDL_GPU_STENCILOP_KEEP;
			op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
			op.pass_op = (stencil == StencilState::WriteReplace ? SDL_GPU_STENCILOP_REPLACE
					: stencil == StencilState::WriteIncrement   ? SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP
																: SDL_GPU_STENCILOP_KEEP);
			op.compare_op = (stencil == StencilState::TestEqual ? SDL_GPU_COMPAREOP_EQUAL : SDL_GPU_COMPAREOP_ALWAYS);
			info.depth_stencil_state.enable_stencil_test = true;
			info.depth_stencil_state.front_stencil_state = op;
			info.depth_stencil_state.back_stencil_state = op;
			info.depth_stencil_state.compare_mask = 0xff;
			info.depth_stencil_state.write_mask = 0xff;
		}
	}

	pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
	if (!pipeline)
	{
		Log::Message(Log::LT_ERROR, "Failed to create pipeline: %s", SDL_GetError());
		RMLUI_ERROR;
	}
	return pipeline;
}

RenderInterface_SDL_GPU::RenderTargets* RenderInterface_SDL_GPU::GetRenderTargets(int new_width, int new_height)
{
	for (Rml::UniquePtr<RenderTargets>& candidate : render_targets)
	{
		if (candidate->width == new_width && candidate->height == new_height)
		{
			candidate->last_used_frame = frame_number;
			return candidate.get();
		}
	}
	if (render_targets.size() >= MaxNumRenderTargets)
	{
		auto oldest = std::min_element(render_targets.begin(), render_targets.end(),
			[](const Rml::UniquePtr<RenderTargets>& a, const Rml::UniquePtr<RenderTargets>& b) { return a->last_used_frame < b->last_used_frame; });
		ReleaseRenderTargets(**oldest);
		render_targets.erase(oldest);
	}
	Rml::UniquePtr<RenderTargets> created = Rml::MakeUnique<RenderTargets>();
	created->width = new_width;
	created->height = new_height;
	created->last_used_frame = frame_number;
	render_targets.push_back(std::move(created));
	return render_targets.back().get();
}

void RenderInterface_SDL_GPU::ReleaseRenderTargets(RenderTargets& render_target)
{
	// SDL releases textures once the GPU is done with them.
	for (SDL_GPUTexture* texture : render_target.layers)
		SDL_ReleaseGPUTexture(device, texture);
	render_target.layers.clear();
	if (render_target.depth_stencil)
		SDL_ReleaseGPUTexture(device, render_target.depth_stencil);
	render_target.depth_stencil = nullptr;
	for (SDL_GPUTexture*& texture : render_target.postprocess)
	{
		if (texture)
			SDL_ReleaseGPUTexture(device, texture);
		texture = nullptr;
	}
}

SDL_GPUTexture* RenderInterface_SDL_GPU::GetLayer(int layer)
{
	RMLUI_ASSERT(targets && layer >= 0);
	if (!targets->depth_stencil)
		targets->depth_stencil =
			CreateTexture(device, depth_stencil_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, targets->width, targets->height, sample_count);
	while (int(targets->layers.size()) <= layer)
	{
		const SDL_GPUTextureUsageFlags usage =
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (sample_count == SDL_GPU_SAMPLECOUNT_1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
		targets->layers.push_back(CreateTexture(device, LayerFormat, usage, targets->width, targets->height, sample_count));
	}
	return targets->layers[size_t(layer)];
}

SDL_GPUTexture* RenderInterface_SDL_GPU::GetPostprocess(int role)
{
	SDL_GPUTexture*& texture = targets->postprocess[postprocess_order[role]];
	if (!texture)
		texture =
			CreateTexture(device, LayerFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, targets->width, targets->height);
	return texture;
}

void RenderInterface_SDL_GPU::SwapPostprocessPrimarySecondary()
{
	std::swap(postprocess_order[0], postprocess_order[1]);
}

void RenderInterface_SDL_GPU::BeginFrame(SDL_GPUCommandBuffer* new_command_buffer, SDL_GPUTexture* swapchain_texture, uint32_t new_width,
	uint32_t new_height)
{
	RMLUI_ASSERT(!in_frame);
	command_buffer = new_command_buffer;
	target_texture = swapchain_texture;
	width = Math::Max(int(new_width), 1);
	height = Math::Max(int(new_height), 1);
	in_frame = true;
	frame_number += 1;

	projection = Matrix4f::ProjectOrtho(0.0f, float(width), float(height), 0.0f, -10'000.f, 10'000.f);
	targets = GetRenderTargets(width, height);

	// The base layer, cleared together with the stencil buffer.
	GetLayer(0);
	num_layers = 1;
	state = DrawState{0, Rectanglei::MakeInvalid(), projection, false, 1};
	commands.push_back([this] { BeginLayerPass(0, true, true); });
}

void RenderInterface_SDL_GPU::EndFrame()
{
	RMLUI_ASSERT(in_frame && num_layers == 1);
	commands.push_back([this] {
		// Composite the base layer onto the target, as the OpenGL 3 renderer does onto its backbuffer.
		ResolveLayer(0, GetPostprocess(0));
		BeginDraw(target_texture, TargetKind::Output, ProgramId::Passthrough, BlendState::Premultiplied, StencilState::Disabled,
			Rectanglei::MakeInvalid());
		DrawQuad(GetPostprocess(0));
	});

	ExecuteCommands();
	num_layers = 0;
	in_frame = false;
	command_buffer = nullptr;
	target_texture = nullptr;
}

void RenderInterface_SDL_GPU::ExecuteCommands()
{
	if (copy_pass)
	{
		SDL_EndGPUCopyPass(copy_pass);
		copy_pass = nullptr;
	}
	postprocess_order[0] = 0;
	postprocess_order[1] = 1;
	postprocess_order[2] = 2;
	postprocess_order[3] = 3;
	for (std::function<void()>& command : commands)
		command();
	commands.clear();
	EndPass();
}

void RenderInterface_SDL_GPU::EndPass()
{
	if (render_pass)
	{
		SDL_EndGPURenderPass(render_pass);
		render_pass = nullptr;
		pass_texture = nullptr;
	}
}

void RenderInterface_SDL_GPU::BeginLayerPass(int layer, bool clear_color, bool clear_stencil)
{
	EndPass();
	SDL_GPUColorTargetInfo color{};
	color.texture = GetLayer(layer);
	color.load_op = (clear_color ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD);
	color.store_op = SDL_GPU_STOREOP_STORE;
	color.clear_color = {0.f, 0.f, 0.f, 0.f};
	SDL_GPUDepthStencilTargetInfo depth_stencil{};
	depth_stencil.texture = targets->depth_stencil;
	depth_stencil.load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth_stencil.store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth_stencil.stencil_load_op = (clear_stencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD);
	depth_stencil.stencil_store_op = SDL_GPU_STOREOP_STORE;
	depth_stencil.clear_stencil = 0;
	render_pass = SDL_BeginGPURenderPass(command_buffer, &color, 1, &depth_stencil);
	if (!render_pass)
		Log::Message(Log::LT_ERROR, "Failed to begin render pass: %s", SDL_GetError());
	pass_texture = color.texture;
}

void RenderInterface_SDL_GPU::BeginDraw(SDL_GPUTexture* target, TargetKind kind, ProgramId program, BlendState blend, StencilState stencil,
	Rectanglei scissor)
{
	if (!render_pass || pass_texture != target)
	{
		if (kind == TargetKind::Layer)
		{
			const auto it = std::find(targets->layers.begin(), targets->layers.end(), target);
			RMLUI_ASSERT(it != targets->layers.end());
			BeginLayerPass(int(it - targets->layers.begin()), false, false);
		}
		else
		{
			EndPass();
			SDL_GPUColorTargetInfo color{};
			color.texture = target;
			color.load_op = SDL_GPU_LOADOP_LOAD;
			color.store_op = SDL_GPU_STOREOP_STORE;
			render_pass = SDL_BeginGPURenderPass(command_buffer, &color, 1, nullptr);
			if (!render_pass)
				Log::Message(Log::LT_ERROR, "Failed to begin render pass: %s", SDL_GetError());
			pass_texture = target;
		}
	}
	SDL_BindGPUGraphicsPipeline(render_pass, GetPipeline(program, kind, blend, stencil));
	SetViewport(Rectanglef::FromSize(Vector2f(float(width), float(height))));
	const Rectanglei clamped = ClampToTarget(scissor.Valid() ? scissor : Rectanglei::FromSize({width, height}));
	const SDL_Rect rect = {clamped.Left(), clamped.Top(), clamped.Width(), clamped.Height()};
	SDL_SetGPUScissor(render_pass, &rect);
}

void RenderInterface_SDL_GPU::SetViewport(Rectanglef viewport)
{
	const SDL_GPUViewport sdl_viewport = {viewport.Left(), viewport.Top(), viewport.Width(), viewport.Height(), 0.f, 1.f};
	SDL_SetGPUViewport(render_pass, &sdl_viewport);
}

void RenderInterface_SDL_GPU::DrawGeometry(const GeometryView* geometry)
{
	if (!geometry || geometry->num_indices == 0)
		return;
	SDL_GPUBufferBinding vertex_buffer_binding{};
	SDL_GPUBufferBinding index_buffer_binding{};
	vertex_buffer_binding.buffer = geometry->vertex_buffer->buffer;
	index_buffer_binding.buffer = geometry->index_buffer->buffer;
	SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_buffer_binding, 1);
	SDL_BindGPUIndexBuffer(render_pass, &index_buffer_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
	SDL_DrawGPUIndexedPrimitives(render_pass, Uint32(geometry->num_indices), 1, 0, 0, 0);
}

void RenderInterface_SDL_GPU::DrawQuad(SDL_GPUTexture* source, Vector2f uv_scale, Vector2f uv_offset)
{
	const TexCoordTransformUniforms transform = {uv_scale, uv_offset};
	SDL_PushGPUVertexUniformData(command_buffer, 0, &transform, sizeof(transform));
	if (source)
	{
		SDL_GPUTextureSamplerBinding binding{source, clamp_sampler};
		SDL_BindGPUFragmentSamplers(render_pass, 0, &binding, 1);
	}
	DrawGeometry(fullscreen_quad);
}

void RenderInterface_SDL_GPU::DrawQuadIn(Rectanglei region, SDL_GPUTexture* source)
{
	const Rectanglei clamped = ClampToTarget(region.Valid() ? region : Rectanglei::FromSize({width, height}));
	if (clamped.Width() <= 0 || clamped.Height() <= 0)
		return;
	const Vector2f size = {float(width), float(height)};
	SetViewport(Rectanglef(clamped));
	DrawQuad(source, Vector2f(clamped.Size()) / size, Vector2f(clamped.TopLeft()) / size);
}

void RenderInterface_SDL_GPU::ResolveLayer(int layer, SDL_GPUTexture* destination)
{
	EndPass();
	if (sample_count == SDL_GPU_SAMPLECOUNT_1)
	{
		CopyTexture(GetLayer(layer), Rectanglei::FromSize({width, height}), destination);
		return;
	}
	SDL_GPUColorTargetInfo color{};
	color.texture = GetLayer(layer);
	color.load_op = SDL_GPU_LOADOP_LOAD;
	color.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
	color.resolve_texture = destination;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &color, 1, nullptr);
	if (!pass)
	{
		Log::Message(Log::LT_ERROR, "Failed to resolve layer: %s", SDL_GetError());
		return;
	}
	SDL_EndGPURenderPass(pass);
}

void RenderInterface_SDL_GPU::CopyTexture(SDL_GPUTexture* source, Rectanglei region, SDL_GPUTexture* destination)
{
	EndPass();
	if (region.Width() <= 0 || region.Height() <= 0)
		return;
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command_buffer);
	if (!pass)
	{
		Log::Message(Log::LT_ERROR, "Failed to begin copy pass: %s", SDL_GetError());
		return;
	}
	SDL_GPUTextureLocation from{};
	from.texture = source;
	from.x = Uint32(region.Left());
	from.y = Uint32(region.Top());
	SDL_GPUTextureLocation to{};
	to.texture = destination;
	SDL_CopyGPUTextureToTexture(pass, &from, &to, Uint32(region.Width()), Uint32(region.Height()), 1, false);
	SDL_EndGPUCopyPass(pass);
}

Rectanglei RenderInterface_SDL_GPU::ClampToTarget(Rectanglei region) const
{
	const Rectanglei bounds = Rectanglei::FromSize({width, height});
	if (!region.Valid() || !region.Intersects(bounds))
		return Rectanglei::FromSize({0, 0});
	return region.Intersect(bounds);
}

Rml::CompiledGeometryHandle RenderInterface_SDL_GPU::CompileGeometry(Span<const Vertex> vertices, Span<const int> indices)
{
	if (in_frame)
	{
		// Uploads happen in one copy pass before the frame's commands run.
		if (!copy_pass)
			copy_pass = SDL_BeginGPUCopyPass(command_buffer);
		if (!copy_pass)
		{
			Log::Message(Log::LT_ERROR, "Failed to begin copy pass: %s", SDL_GetError());
			return 0;
		}
		return reinterpret_cast<Rml::CompiledGeometryHandle>(UploadGeometry(copy_pass, vertices, indices));
	}

	// Outside a frame, e.g. while RmlUi updates a document: upload with a command buffer of its own.
	SDL_GPUCommandBuffer* upload_command_buffer = SDL_AcquireGPUCommandBuffer(device);
	if (!upload_command_buffer)
	{
		Log::Message(Log::LT_ERROR, "Failed to acquire command buffer: %s", SDL_GetError());
		return 0;
	}
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(upload_command_buffer);
	GeometryView* geometry = pass ? UploadGeometry(pass, vertices, indices) : nullptr;
	if (pass)
		SDL_EndGPUCopyPass(pass);
	SDL_SubmitGPUCommandBuffer(upload_command_buffer);
	return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

RenderInterface_SDL_GPU::GeometryView* RenderInterface_SDL_GPU::UploadGeometry(SDL_GPUCopyPass* pass, Span<const Vertex> vertices,
	Span<const int> indices)
{
	GeometryView* geometry = new GeometryView();
	geometry->num_indices = 0;
	geometry->vertex_buffer = nullptr;
	geometry->index_buffer = nullptr;
	if (vertices.empty() || indices.empty())
		return geometry;

	const Uint32 vertex_size = Uint32(vertices.size() * sizeof(Vertex));
	const Uint32 index_size = Uint32(indices.size() * sizeof(int));
	geometry->vertex_buffer = RequestBuffer(int(vertex_size), SDL_GPU_BUFFERUSAGE_VERTEX);
	geometry->index_buffer = RequestBuffer(int(index_size), SDL_GPU_BUFFERUSAGE_INDEX);
	if (!geometry->vertex_buffer || !geometry->index_buffer)
	{
		Log::Message(Log::LT_ERROR, "Failed to request buffer(s)");
		delete geometry;
		return nullptr;
	}

	void* vertex_data = SDL_MapGPUTransferBuffer(device, geometry->vertex_buffer->transfer_buffer, true);
	void* index_data = SDL_MapGPUTransferBuffer(device, geometry->index_buffer->transfer_buffer, true);
	if (!vertex_data || !index_data)
	{
		Log::Message(Log::LT_ERROR, "Failed to map transfer buffer(s): %s", SDL_GetError());
		delete geometry;
		return nullptr;
	}

	std::memcpy(vertex_data, vertices.data(), vertex_size);
	std::memcpy(index_data, indices.data(), index_size);
	SDL_UnmapGPUTransferBuffer(device, geometry->vertex_buffer->transfer_buffer);
	SDL_UnmapGPUTransferBuffer(device, geometry->index_buffer->transfer_buffer);

	SDL_GPUTransferBufferLocation location{};
	SDL_GPUBufferRegion region{};

	location.transfer_buffer = geometry->vertex_buffer->transfer_buffer;
	region.buffer = geometry->vertex_buffer->buffer;
	region.size = vertex_size;
	SDL_UploadToGPUBuffer(pass, &location, &region, false);

	location.transfer_buffer = geometry->index_buffer->transfer_buffer;
	region.buffer = geometry->index_buffer->buffer;
	region.size = index_size;
	SDL_UploadToGPUBuffer(pass, &location, &region, false);

	geometry->num_indices = int(indices.size());
	geometry->vertex_buffer->in_use = true;
	geometry->index_buffer->in_use = true;
	return geometry;
}

void RenderInterface_SDL_GPU::ReleaseGeometry(CompiledGeometryHandle handle)
{
	commands.push_back([handle] {
		GeometryView* geometry = reinterpret_cast<GeometryView*>(handle);
		if (!geometry)
			return;
		if (geometry->vertex_buffer)
			geometry->vertex_buffer->in_use = false;
		if (geometry->index_buffer)
			geometry->index_buffer->in_use = false;
		delete geometry;
	});
}

void RenderInterface_SDL_GPU::RenderGeometryWith(ProgramId program, const DrawState& draw_state, const GeometryView* geometry, Vector2f translation,
	SDL_GPUTexture* texture)
{
	const StencilState stencil = (draw_state.clip_mask_enabled ? StencilState::TestEqual : StencilState::Disabled);
	BeginDraw(GetLayer(draw_state.layer), TargetKind::Layer, program, BlendState::Premultiplied, stencil, draw_state.scissor);
	if (stencil != StencilState::Disabled)
		SDL_SetGPUStencilReference(render_pass, Uint8(draw_state.stencil_ref));
	SDL_PushGPUVertexUniformData(command_buffer, 0, &draw_state.transform, sizeof(draw_state.transform));
	SDL_PushGPUVertexUniformData(command_buffer, 1, &translation, sizeof(translation));
	if (texture)
	{
		SDL_GPUTextureSamplerBinding binding{texture, linear_sampler};
		SDL_BindGPUFragmentSamplers(render_pass, 0, &binding, 1);
	}
	DrawGeometry(geometry);
}

void RenderInterface_SDL_GPU::RenderGeometry(CompiledGeometryHandle handle, Vector2f translation, TextureHandle texture)
{
	const DrawState draw_state = state;
	commands.push_back([this, draw_state, handle, translation, texture] {
		RenderGeometryWith(texture ? ProgramId::Texture : ProgramId::Color, draw_state, reinterpret_cast<const GeometryView*>(handle), translation,
			reinterpret_cast<SDL_GPUTexture*>(texture));
	});
}

void RenderInterface_SDL_GPU::EnableScissorRegion(bool enable)
{
	// As in the OpenGL 3 renderer: enabling is followed by SetScissorRegion().
	if (!enable)
		state.scissor = Rectanglei::MakeInvalid();
}

void RenderInterface_SDL_GPU::SetScissorRegion(Rectanglei region)
{
	state.scissor = region;
}

void RenderInterface_SDL_GPU::EnableClipMask(bool enable)
{
	state.clip_mask_enabled = enable;
}

void RenderInterface_SDL_GPU::RenderToClipMask(ClipMaskOperation operation, CompiledGeometryHandle handle, Vector2f translation)
{
	bool clear = false;
	int clear_value = 0;
	int write_value = 1;
	int test_value = 1;
	StencilState write = StencilState::WriteReplace;
	switch (operation)
	{
	case ClipMaskOperation::Set:
		clear = true;
		clear_value = 0;
		write_value = 1;
		break;
	case ClipMaskOperation::SetInverse:
		clear = true;
		clear_value = 1;
		write_value = 0;
		break;
	case ClipMaskOperation::Intersect:
		write = StencilState::WriteIncrement;
		test_value = state.stencil_ref + 1;
		break;
	}

	const DrawState draw_state = state;
	state.stencil_ref = test_value;
	commands.push_back([this, draw_state, clear, clear_value, write, write_value, handle, translation] {
		SDL_GPUTexture* layer = GetLayer(draw_state.layer);
		if (clear)
		{
			// Clear the stencil buffer within the scissor region, as glClear() does.
			BeginDraw(layer, TargetKind::Layer, ProgramId::Fill, BlendState::None, StencilState::WriteReplace, draw_state.scissor);
			SDL_SetGPUStencilReference(render_pass, Uint8(clear_value));
			DrawQuadIn(draw_state.scissor, nullptr);
		}
		BeginDraw(layer, TargetKind::Layer, ProgramId::Color, BlendState::None, write, draw_state.scissor);
		SDL_SetGPUStencilReference(render_pass, Uint8(write_value));
		SDL_PushGPUVertexUniformData(command_buffer, 0, &draw_state.transform, sizeof(draw_state.transform));
		SDL_PushGPUVertexUniformData(command_buffer, 1, &translation, sizeof(translation));
		DrawGeometry(reinterpret_cast<const GeometryView*>(handle));
	});
}

TextureHandle RenderInterface_SDL_GPU::LoadTexture(Vector2i& texture_dimensions, const String& source)
{
	Rml::FileInterface* file_interface = Rml::GetFileInterface();
	Rml::FileHandle file_handle = file_interface->Open(source);
	if (!file_handle)
	{
		return 0;
	}

	file_interface->Seek(file_handle, 0, SEEK_END);
	size_t buffer_size = file_interface->Tell(file_handle);
	file_interface->Seek(file_handle, 0, SEEK_SET);

	using Rml::byte;
	Rml::UniquePtr<byte[]> buffer(new byte[buffer_size]);
	file_interface->Read(buffer.get(), buffer_size, file_handle);
	file_interface->Close(file_handle);

	const size_t i_ext = source.rfind('.');
	Rml::String extension = (i_ext == Rml::String::npos ? Rml::String() : source.substr(i_ext + 1));

	SDL_Surface* surface = IMG_LoadTyped_IO(SDL_IOFromMem(buffer.get(), int(buffer_size)), 1, extension.c_str());
	if (!surface)
	{
		return 0;
	}

	texture_dimensions = {surface->w, surface->h};

	if (surface->format != SDL_PIXELFORMAT_RGBA32)
	{
		SDL_Surface* converted_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(surface);
		if (!converted_surface)
		{
			return 0;
		}

		surface = converted_surface;
	}

	// Convert colors to premultiplied alpha, which is necessary for correct alpha compositing.
	const size_t pixels_byte_size = surface->w * surface->h * 4;
	byte* pixels = static_cast<byte*>(surface->pixels);
	for (size_t i = 0; i < pixels_byte_size; i += 4)
	{
		const byte alpha = pixels[i + 3];
		for (size_t j = 0; j < 3; ++j)
		{
			pixels[i + j] = byte(int(pixels[i + j]) * int(alpha) / 255);
		}
	}

	Span<const byte> data{static_cast<const byte*>(surface->pixels), static_cast<size_t>(surface->pitch * surface->h)};
	texture_dimensions = {surface->w, surface->h};

	TextureHandle handle = GenerateTexture(data, texture_dimensions);

	SDL_DestroySurface(surface);

	return handle;
}

TextureHandle RenderInterface_SDL_GPU::GenerateTexture(Span<const byte> source, Vector2i source_dimensions)
{
	SDL_GPUTransferBuffer* transfer_buffer;
	{
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = source_dimensions.x * source_dimensions.y * 4;
		transfer_buffer = SDL_CreateGPUTransferBuffer(device, &info);
		if (!transfer_buffer)
		{
			Log::Message(Log::LT_ERROR, "Failed to create transfer buffer: %s", SDL_GetError());
			return 0;
		}
	}

	void* dst = SDL_MapGPUTransferBuffer(device, transfer_buffer, false);
	if (!dst)
	{
		SDL_Log("Failed to map transfer buffer: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
		return 0;
	}

	std::memcpy(dst, source.data(), source_dimensions.x * source_dimensions.y * 4);
	SDL_UnmapGPUTransferBuffer(device, transfer_buffer);

	SDL_GPUTexture* texture = CreateTexture(device, LayerFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER, source_dimensions.x, source_dimensions.y);
	if (!texture)
	{
		SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
		return 0;
	}

	SDL_GPUTextureTransferInfo transfer_info{};
	SDL_GPUTextureRegion region{};
	transfer_info.transfer_buffer = transfer_buffer;
	region.texture = texture;
	region.w = source_dimensions.x;
	region.h = source_dimensions.y;
	region.d = 1;

	// We can get calls out outside of Begin/End Frame so always acquire a command buffer here
	SDL_GPUCommandBuffer* upload_command_buffer = SDL_AcquireGPUCommandBuffer(device);
	if (!upload_command_buffer)
	{
		Log::Message(Log::LT_ERROR, "Failed to acquire command buffer: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
		SDL_ReleaseGPUTexture(device, texture);
		return 0;
	}

	SDL_GPUCopyPass* upload_pass = SDL_BeginGPUCopyPass(upload_command_buffer);
	if (!upload_pass)
	{
		Log::Message(Log::LT_ERROR, "Failed to begin copy pass: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
		SDL_ReleaseGPUTexture(device, texture);
		SDL_CancelGPUCommandBuffer(upload_command_buffer);
		return 0;
	}

	SDL_UploadToGPUTexture(upload_pass, &transfer_info, &region, false);
	SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
	SDL_EndGPUCopyPass(upload_pass);
	SDL_SubmitGPUCommandBuffer(upload_command_buffer);

	return reinterpret_cast<TextureHandle>(texture);
}

void RenderInterface_SDL_GPU::ReleaseTexture(TextureHandle texture_handle)
{
	commands.push_back([this, texture_handle] { SDL_ReleaseGPUTexture(device, reinterpret_cast<SDL_GPUTexture*>(texture_handle)); });
}

void RenderInterface_SDL_GPU::SetTransform(const Rml::Matrix4f* new_transform)
{
	state.transform = (new_transform ? projection * (*new_transform) : projection);
}

LayerHandle RenderInterface_SDL_GPU::PushLayer()
{
	const int layer = num_layers;
	GetLayer(layer);
	num_layers += 1;
	state.layer = layer;
	commands.push_back([this, layer] { BeginLayerPass(layer, true, false); });
	return LayerHandle(layer);
}

void RenderInterface_SDL_GPU::CompositeLayers(LayerHandle source, LayerHandle destination, BlendMode blend_mode,
	Span<const CompiledFilterHandle> filters)
{
	const DrawState draw_state = state;
	const Rml::Vector<CompiledFilterHandle> filter_list(filters.begin(), filters.end());
	commands.push_back([this, draw_state, source, destination, blend_mode, filter_list] {
		// Resolve the source layer whether or not there are filters, as the OpenGL 3 renderer does.
		ResolveLayer(int(source), GetPostprocess(0));
		RenderFilters({filter_list.data(), filter_list.size()}, draw_state.scissor);

		const StencilState stencil = (draw_state.clip_mask_enabled ? StencilState::TestEqual : StencilState::Disabled);
		BeginDraw(GetLayer(int(destination)), TargetKind::Layer, ProgramId::Passthrough,
			blend_mode == BlendMode::Replace ? BlendState::None : BlendState::Premultiplied, stencil, draw_state.scissor);
		if (stencil != StencilState::Disabled)
			SDL_SetGPUStencilReference(render_pass, Uint8(draw_state.stencil_ref));
		DrawQuadIn(draw_state.scissor, GetPostprocess(0));
	});
}

void RenderInterface_SDL_GPU::PopLayer()
{
	RMLUI_ASSERT(num_layers > 1);
	num_layers -= 1;
	state.layer = num_layers - 1;
}

TextureHandle RenderInterface_SDL_GPU::SaveLayerAsTexture()
{
	RMLUI_ASSERT(state.scissor.Valid());
	const Rectanglei bounds = ClampToTarget(state.scissor);
	if (bounds.Width() <= 0 || bounds.Height() <= 0)
		return {};

	SDL_GPUTexture* texture = CreateTexture(device, LayerFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER, bounds.Width(), bounds.Height());
	if (!texture)
		return {};

	const int layer = state.layer;
	commands.push_back([this, layer, bounds, texture] {
		ResolveLayer(layer, GetPostprocess(0));
		CopyTexture(GetPostprocess(0), bounds, texture);
	});
	return reinterpret_cast<TextureHandle>(texture);
}

CompiledFilterHandle RenderInterface_SDL_GPU::SaveLayerAsMaskImage()
{
	const int layer = state.layer;
	commands.push_back([this, layer] {
		ResolveLayer(layer, GetPostprocess(0));
		CopyTexture(GetPostprocess(0), Rectanglei::FromSize({width, height}), GetPostprocess(3));
	});

	CompiledFilter filter = {};
	filter.type = FilterType::MaskImage;
	return reinterpret_cast<CompiledFilterHandle>(new CompiledFilter(std::move(filter)));
}

CompiledFilterHandle RenderInterface_SDL_GPU::CompileFilter(const String& name, const Dictionary& parameters)
{
	CompiledFilter filter = {};

	if (name == "opacity")
	{
		filter.type = FilterType::Passthrough;
		filter.blend_factor = Rml::Get(parameters, "value", 1.0f);
	}
	else if (name == "blur")
	{
		filter.type = FilterType::Blur;
		filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
	}
	else if (name == "drop-shadow")
	{
		filter.type = FilterType::DropShadow;
		filter.sigma = Rml::Get(parameters, "sigma", 0.f);
		filter.color = Rml::Get(parameters, "color", Colourb()).ToPremultiplied();
		filter.offset = Rml::Get(parameters, "offset", Vector2f(0.f));
	}
	else if (name == "brightness")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		filter.color_matrix = Matrix4f::Diag(value, value, value, 1.f);
	}
	else if (name == "contrast")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float grayness = 0.5f - 0.5f * value;
		filter.color_matrix = Matrix4f::Diag(value, value, value, 1.f);
		filter.color_matrix.SetColumn(3, Vector4f(grayness, grayness, grayness, 1.f));
	}
	else if (name == "invert")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
		const float inverted = 1.f - 2.f * value;
		filter.color_matrix = Matrix4f::Diag(inverted, inverted, inverted, 1.f);
		filter.color_matrix.SetColumn(3, Vector4f(value, value, value, 1.f));
	}
	else if (name == "grayscale")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float rev_value = 1.f - value;
		const Vector3f gray = value * Vector3f(0.2126f, 0.7152f, 0.0722f);
		// clang-format off
		filter.color_matrix = Matrix4f::FromRows(
			{gray.x + rev_value, gray.y,             gray.z,             0.f},
			{gray.x,             gray.y + rev_value, gray.z,             0.f},
			{gray.x,             gray.y,             gray.z + rev_value, 0.f},
			{0.f,                0.f,                0.f,                1.f}
		);
		// clang-format on
	}
	else if (name == "sepia")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float rev_value = 1.f - value;
		const Vector3f r_mix = value * Vector3f(0.393f, 0.769f, 0.189f);
		const Vector3f g_mix = value * Vector3f(0.349f, 0.686f, 0.168f);
		const Vector3f b_mix = value * Vector3f(0.272f, 0.534f, 0.131f);
		// clang-format off
		filter.color_matrix = Matrix4f::FromRows(
			{r_mix.x + rev_value, r_mix.y,             r_mix.z,             0.f},
			{g_mix.x,             g_mix.y + rev_value, g_mix.z,             0.f},
			{b_mix.x,             b_mix.y,             b_mix.z + rev_value, 0.f},
			{0.f,                 0.f,                 0.f,                 1.f}
		);
		// clang-format on
	}
	else if (name == "hue-rotate")
	{
		// Hue-rotation and saturation values based on: https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float s = Math::Sin(value);
		const float c = Math::Cos(value);
		// clang-format off
		filter.color_matrix = Matrix4f::FromRows(
			{0.213f + 0.787f * c - 0.213f * s,  0.715f - 0.715f * c - 0.715f * s,  0.072f - 0.072f * c + 0.928f * s,  0.f},
			{0.213f - 0.213f * c + 0.143f * s,  0.715f + 0.285f * c + 0.140f * s,  0.072f - 0.072f * c - 0.283f * s,  0.f},
			{0.213f - 0.213f * c - 0.787f * s,  0.715f - 0.715f * c + 0.715f * s,  0.072f + 0.928f * c + 0.072f * s,  0.f},
			{0.f,                               0.f,                               0.f,                               1.f}
		);
		// clang-format on
	}
	else if (name == "saturate")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		// clang-format off
		filter.color_matrix = Matrix4f::FromRows(
			{0.213f + 0.787f * value,  0.715f - 0.715f * value,  0.072f - 0.072f * value,  0.f},
			{0.213f - 0.213f * value,  0.715f + 0.285f * value,  0.072f - 0.072f * value,  0.f},
			{0.213f - 0.213f * value,  0.715f - 0.715f * value,  0.072f + 0.928f * value,  0.f},
			{0.f,                      0.f,                      0.f,                      1.f}
		);
		// clang-format on
	}

	if (filter.type != FilterType::Invalid)
		return reinterpret_cast<CompiledFilterHandle>(new CompiledFilter(std::move(filter)));

	Log::Message(Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
	return {};
}

void RenderInterface_SDL_GPU::ReleaseFilter(CompiledFilterHandle filter)
{
	// Compositing commands that use it may still be waiting to run.
	commands.push_back([filter] { delete reinterpret_cast<CompiledFilter*>(filter); });
}

void RenderInterface_SDL_GPU::RenderFilters(Span<const CompiledFilterHandle> filter_handles, Rectanglei scissor)
{
	const Rectanglei window = ClampToTarget(scissor.Valid() ? scissor : Rectanglei::FromSize({width, height}));
	const Vector2f size = Vector2f(float(width), float(height));

	for (const CompiledFilterHandle filter_handle : filter_handles)
	{
		const CompiledFilter& filter = *reinterpret_cast<const CompiledFilter*>(filter_handle);
		switch (filter.type)
		{
		case FilterType::Passthrough:
		{
			BeginDraw(GetPostprocess(1), TargetKind::Postprocess, ProgramId::Passthrough, BlendState::Constant, StencilState::Disabled, window);
			const SDL_FColor factor = {filter.blend_factor, filter.blend_factor, filter.blend_factor, filter.blend_factor};
			SDL_SetGPUBlendConstants(render_pass, factor);
			DrawQuad(GetPostprocess(0));
			SwapPostprocessPrimarySecondary();
		}
		break;
		case FilterType::Blur:
		{
			RenderBlur(filter.sigma, GetPostprocess(0), GetPostprocess(1), window);
		}
		break;
		case FilterType::DropShadow:
		{
			DropShadowUniforms uniforms = {};
			uniforms.color = ConvertToColorf(filter.color);
			SetTexCoordLimits(uniforms.tex_coord_min, uniforms.tex_coord_max, window, {width, height});
			BeginDraw(GetPostprocess(1), TargetKind::Postprocess, ProgramId::DropShadow, BlendState::None, StencilState::Disabled, window);
			SDL_PushGPUFragmentUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
			// Draw the shadow where the offset puts it: read the texture that far back.
			DrawQuad(GetPostprocess(0), Vector2f(1.f), -filter.offset / size);

			if (filter.sigma >= 0.5f)
				RenderBlur(filter.sigma, GetPostprocess(1), GetPostprocess(2), window);

			BeginDraw(GetPostprocess(1), TargetKind::Postprocess, ProgramId::Passthrough, BlendState::Premultiplied, StencilState::Disabled, window);
			DrawQuad(GetPostprocess(0));
			SwapPostprocessPrimarySecondary();
		}
		break;
		case FilterType::ColorMatrix:
		{
			BeginDraw(GetPostprocess(1), TargetKind::Postprocess, ProgramId::ColorMatrix, BlendState::None, StencilState::Disabled, window);
			if (std::is_same<Matrix4f, RowMajorMatrix4f>::value)
				SDL_PushGPUFragmentUniformData(command_buffer, 0, filter.color_matrix.Transpose().data(), sizeof(Matrix4f));
			else
				SDL_PushGPUFragmentUniformData(command_buffer, 0, filter.color_matrix.data(), sizeof(Matrix4f));
			DrawQuad(GetPostprocess(0));
			SwapPostprocessPrimarySecondary();
		}
		break;
		case FilterType::MaskImage:
		{
			BeginDraw(GetPostprocess(1), TargetKind::Postprocess, ProgramId::BlendMask, BlendState::None, StencilState::Disabled, window);
			const SDL_GPUTextureSamplerBinding bindings[2] = {{GetPostprocess(0), clamp_sampler}, {GetPostprocess(3), clamp_sampler}};
			SDL_BindGPUFragmentSamplers(render_pass, 0, bindings, 2);
			DrawQuad(nullptr);
			SwapPostprocessPrimarySecondary();
		}
		break;
		case FilterType::Invalid:
		{
			Log::Message(Log::LT_WARNING, "Unhandled render filter %d.", (int)filter.type);
		}
		break;
		}
	}
}

void RenderInterface_SDL_GPU::RenderBlur(float sigma, SDL_GPUTexture* source_destination, SDL_GPUTexture* temp, const Rectanglei window)
{
	RMLUI_ASSERT(source_destination != temp);

	int pass_level = 0;
	SigmaToParameters(sigma, pass_level, sigma);

	const Vector2f size = Vector2f(float(width), float(height));

	// Begin by downscaling so that the blur pass can be done at a reduced resolution for large sigma.
	Rectanglei scissor = window;

	// Scale UVs if we have even dimensions, such that texture fetches align perfectly between texels, thereby producing a 50% blend of
	// neighboring texels.
	const Vector2f uv_scaling = {(width % 2 == 1) ? (1.f - 1.f / size.x) : 1.f, (height % 2 == 1) ? (1.f - 1.f / size.y) : 1.f};

	// Downscale by iterative half-scaling with bilinear filtering, to reduce aliasing.
	for (int i = 0; i < pass_level; i++)
	{
		scissor.p0 = (scissor.p0 + Vector2i(1)) / 2;
		scissor.p1 = Math::Max(scissor.p1 / 2, scissor.p0);
		const bool from_source = (i % 2 == 0);
		SDL_GPUTexture* destination = (from_source ? temp : source_destination);
		BeginDraw(destination, TargetKind::Postprocess, ProgramId::Passthrough, BlendState::None, StencilState::Disabled, scissor);
		SetViewport(Rectanglef::FromSize(Vector2f(float(width / 2), float(height / 2))));
		DrawQuad(from_source ? source_destination : temp, uv_scaling);
	}

	// Ensure texture data end up in the temp buffer. Depending on the last downscaling, we might need to move it from the source_destination buffer.
	if (pass_level % 2 == 0)
	{
		BeginDraw(temp, TargetKind::Postprocess, ProgramId::Passthrough, BlendState::None, StencilState::Disabled, scissor);
		DrawQuad(source_destination);
	}

	BlurUniforms uniforms = {};
	SetBlurWeights(uniforms.weights, sigma);
	SetTexCoordLimits(uniforms.tex_coord_min, uniforms.tex_coord_max, scissor, {width, height});

	auto BlurPass = [&](SDL_GPUTexture* from, SDL_GPUTexture* to, Vector2f texel_offset) {
		BeginDraw(to, TargetKind::Postprocess, ProgramId::Blur, BlendState::None, StencilState::Disabled, scissor);
		const Vector4f offset(texel_offset.x, texel_offset.y, 0.f, 0.f);
		SDL_PushGPUVertexUniformData(command_buffer, 0, &offset, sizeof(offset));
		SDL_PushGPUFragmentUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
		SDL_GPUTextureSamplerBinding binding{from, clamp_sampler};
		SDL_BindGPUFragmentSamplers(render_pass, 0, &binding, 1);
		DrawGeometry(fullscreen_quad);
	};

	// Blur render pass - vertical.
	BlurPass(temp, source_destination, {0.f, 1.f / size.y});

	// Blur render pass - horizontal. Add a 1px transparent border around the blur region by first clearing with a padded
	// scissor, which prevents artifacts when upscaling the blur result in the later step.
	BeginDraw(temp, TargetKind::Postprocess, ProgramId::Fill, BlendState::None, StencilState::Disabled, scissor.Extend(1));
	DrawQuad(nullptr);
	BlurPass(source_destination, temp, {1.f / size.x, 0.f});

	// Upscale the blurred image into the window, by drawing the scissor region of the temp buffer over it.
	const Vector2i src_min = scissor.p0;
	const Vector2i src_max = scissor.p1;
	auto Upscale = [&](Vector2i to_min, Vector2i to_max) {
		BeginDraw(source_destination, TargetKind::Postprocess, ProgramId::Passthrough, BlendState::None, StencilState::Disabled, window);
		SetViewport(Rectanglef::FromCorners(Vector2f(to_min), Vector2f(to_max)));
		DrawQuad(temp, Vector2f(src_max - src_min) / size, Vector2f(src_min) / size);
	};
	Upscale(window.p0, window.p1);

	// The above upscale might be jittery at low resolutions (large pass levels). For more stable and accurate rendering
	// we next upscale the blur image by an exact power-of-two. However, this may not fill the edges completely so we need
	// to do the above first.
	const Vector2i target_min = src_min * (1 << pass_level);
	const Vector2i target_max = src_max * (1 << pass_level);
	if (target_min != window.p0 || target_max != window.p1)
		Upscale(target_min, target_max);
}

CompiledShaderHandle RenderInterface_SDL_GPU::CompileShader(const String& name, const Dictionary& parameters)
{
	auto ApplyColorStopList = [](GradientUniforms& gradient, const Dictionary& shader_parameters) {
		auto it = shader_parameters.find("color_stop_list");
		RMLUI_ASSERT(it != shader_parameters.end() && it->second.GetType() == Variant::COLORSTOPLIST);
		const ColorStopList& color_stop_list = it->second.GetReference<ColorStopList>();
		const int num_stops = Math::Min((int)color_stop_list.size(), MaxNumStops);

		gradient.num_stops = num_stops;
		for (int i = 0; i < num_stops; i++)
		{
			const ColorStop& stop = color_stop_list[i];
			RMLUI_ASSERT(stop.position.unit == Unit::NUMBER);
			gradient.stop_positions[i] = stop.position.number;
			gradient.stop_colors[i] = ConvertToColorf(stop.color);
		}
	};

	CompiledShader shader = {};

	if (name == "linear-gradient")
	{
		shader.type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader.gradient.function = int(repeating ? ShaderGradientFunction::RepeatingLinear : ShaderGradientFunction::Linear);
		shader.gradient.p = Rml::Get(parameters, "p0", Vector2f(0.f));
		shader.gradient.v = Rml::Get(parameters, "p1", Vector2f(0.f)) - shader.gradient.p;
		ApplyColorStopList(shader.gradient, parameters);
	}
	else if (name == "radial-gradient")
	{
		shader.type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader.gradient.function = int(repeating ? ShaderGradientFunction::RepeatingRadial : ShaderGradientFunction::Radial);
		shader.gradient.p = Rml::Get(parameters, "center", Vector2f(0.f));
		shader.gradient.v = Vector2f(1.f) / Rml::Get(parameters, "radius", Vector2f(1.f));
		ApplyColorStopList(shader.gradient, parameters);
	}
	else if (name == "conic-gradient")
	{
		shader.type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader.gradient.function = int(repeating ? ShaderGradientFunction::RepeatingConic : ShaderGradientFunction::Conic);
		shader.gradient.p = Rml::Get(parameters, "center", Vector2f(0.f));
		const float angle = Rml::Get(parameters, "angle", 0.f);
		shader.gradient.v = {Math::Cos(angle), Math::Sin(angle)};
		ApplyColorStopList(shader.gradient, parameters);
	}

	if (shader.type != CompiledShaderType::Invalid)
		return reinterpret_cast<CompiledShaderHandle>(new CompiledShader(std::move(shader)));

	Log::Message(Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
	return {};
}

void RenderInterface_SDL_GPU::RenderShader(CompiledShaderHandle shader_handle, CompiledGeometryHandle geometry_handle, Vector2f translation,
	TextureHandle /*texture*/)
{
	RMLUI_ASSERT(shader_handle && geometry_handle);
	const CompiledShader& shader = *reinterpret_cast<CompiledShader*>(shader_handle);
	if (shader.type != CompiledShaderType::Gradient)
	{
		Log::Message(Log::LT_WARNING, "Unhandled render shader %d.", (int)shader.type);
		return;
	}

	const DrawState draw_state = state;
	const GradientUniforms uniforms = shader.gradient;
	commands.push_back([this, draw_state, uniforms, geometry_handle, translation] {
		const StencilState stencil = (draw_state.clip_mask_enabled ? StencilState::TestEqual : StencilState::Disabled);
		BeginDraw(GetLayer(draw_state.layer), TargetKind::Layer, ProgramId::Gradient, BlendState::Premultiplied, stencil, draw_state.scissor);
		if (stencil != StencilState::Disabled)
			SDL_SetGPUStencilReference(render_pass, Uint8(draw_state.stencil_ref));
		SDL_PushGPUVertexUniformData(command_buffer, 0, &draw_state.transform, sizeof(draw_state.transform));
		SDL_PushGPUVertexUniformData(command_buffer, 1, &translation, sizeof(translation));
		SDL_PushGPUFragmentUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
		DrawGeometry(reinterpret_cast<const GeometryView*>(geometry_handle));
	});
}

void RenderInterface_SDL_GPU::ReleaseShader(CompiledShaderHandle shader_handle)
{
	commands.push_back([shader_handle] { delete reinterpret_cast<CompiledShader*>(shader_handle); });
}

RenderInterface_SDL_GPU::Buffer* RenderInterface_SDL_GPU::RequestBuffer(int capacity, SDL_GPUBufferUsageFlags usage)
{
	auto it = std::lower_bound(buffers.begin(), buffers.end(), capacity,
		[](const Rml::UniquePtr<Buffer>& lhs, int capacity) { return lhs->capacity < capacity; });

	for (auto tmp_it = it; tmp_it != buffers.end(); ++tmp_it)
	{
		const auto& buffer = *tmp_it;
		if (!buffer->in_use && buffer->usage == usage)
		{
			// set in_use as false and expect the caller to set it to true themselves
			buffer->in_use = false;
			return buffer.get();
		}
	}

	Rml::UniquePtr<Buffer> buffer = Rml::MakeUnique<Buffer>();
	{
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = capacity;
		buffer->transfer_buffer = SDL_CreateGPUTransferBuffer(device, &info);
	}
	{
		SDL_GPUBufferCreateInfo info{};
		info.usage = usage;
		info.size = capacity;
		buffer->buffer = SDL_CreateGPUBuffer(device, &info);
	}
	if (!buffer->transfer_buffer || !buffer->buffer)
	{
		Log::Message(Log::LT_ERROR, "Failed to create buffer(s): %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(device, buffer->transfer_buffer);
		SDL_ReleaseGPUBuffer(device, buffer->buffer);
		return {};
	}
	buffer->usage = usage;
	buffer->in_use = false;
	buffer->capacity = capacity;
	auto inserted_it = buffers.insert(it, std::move(buffer));
	return inserted_it->get();
}
