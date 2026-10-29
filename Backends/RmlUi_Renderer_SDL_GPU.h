#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <SDL3/SDL.h>
#include <functional>

// The default multisampling of the layers, as RMLUI_NUM_MSAA_SAMPLES in the OpenGL 3 renderer.
#ifndef RMLUI_SDL_GPU_MSAA_SAMPLES
	#define RMLUI_SDL_GPU_MSAA_SAMPLES 2
#endif

/*
    Renders RmlUi through SDL_GPU, with the same features as the OpenGL 3 renderer: geometry and textures with
    scissoring and transforms, clip masks, layers with filters (blur, drop shadow, color matrices, opacity and masks),
    and gradient shaders.

    Like the OpenGL 3 renderer, it draws into multisampled layers that share a stencil buffer, resolves them into
    postprocess textures for filters, and composites the base layer onto the target in EndFrame(). Calls between
    BeginFrame() and EndFrame() only record commands; EndFrame() runs them, after all uploads of the frame, so that
    uploads and drawing need no interleaved copy passes.
*/
class RenderInterface_SDL_GPU : public Rml::RenderInterface {
public:
	// Multisamples the layers msaa_samples times, 1 for not at all. Devices that lack that count take the next larger
	// one they have, else the next smaller one.
	RenderInterface_SDL_GPU(SDL_GPUDevice* device, SDL_Window* window, int msaa_samples = RMLUI_SDL_GPU_MSAA_SAMPLES);
	void Shutdown();

	// The samples per pixel of the layers that the device gave.
	int GetMsaaSamples() const;

	// Draws into the target texture, which must have the window's swapchain format and the given size. The target is
	// expected to be cleared: the frame is blended onto it with premultiplied alpha.
	void BeginFrame(SDL_GPUCommandBuffer* command_buffer, SDL_GPUTexture* swapchain_texture, uint32_t width, uint32_t height);
	void EndFrame();

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
	void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture_handle) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

	void SetTransform(const Rml::Matrix4f* new_transform) override;

	Rml::LayerHandle PushLayer() override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
		Rml::Span<const Rml::CompiledFilterHandle> filters) override;
	void PopLayer() override;

	Rml::TextureHandle SaveLayerAsTexture() override;
	Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

	Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle shader_handle) override;

	enum class ProgramId { Color, Texture, Gradient, Passthrough, ColorMatrix, BlendMask, Blur, DropShadow, Fill, Count };
	// What a pipeline draws into: a layer (multisampled, with the stencil buffer), a postprocess texture, or the target.
	enum class TargetKind { Layer, Postprocess, Output, Count };
	enum class BlendState { None, Premultiplied, Constant, Count };
	enum class StencilState { Disabled, TestEqual, WriteReplace, WriteIncrement, Count };

private:
	struct Buffer {
		SDL_GPUTransferBuffer* transfer_buffer;
		SDL_GPUBuffer* buffer;
		SDL_GPUBufferUsageFlags usage;
		int capacity;
		bool in_use;
	};

	struct GeometryView {
		Buffer* vertex_buffer;
		Buffer* index_buffer;
		int num_indices;
	};

	// The textures for frames of one size, until no frame has had that size for a while. Several windows of different
	// sizes can share the renderer.
	struct RenderTargets {
		int width = 0;
		int height = 0;
		Rml::Vector<SDL_GPUTexture*> layers;
		SDL_GPUTexture* depth_stencil = nullptr;
		SDL_GPUTexture* postprocess[4] = {}; // in the order of the postprocess roles, see postprocess_order
		uint64_t last_used_frame = 0;
	};

	// The state that draws record when they are called.
	struct DrawState {
		int layer;
		Rml::Rectanglei scissor;
		Rml::Matrix4f transform;
		bool clip_mask_enabled;
		int stencil_ref;
	};

	SDL_GPUDevice* device;
	SDL_Window* window;
	SDL_GPUTextureFormat output_format;
	SDL_GPUTextureFormat depth_stencil_format;
	SDL_GPUSampleCount sample_count;
	SDL_GPUGraphicsPipeline* pipelines[size_t(ProgramId::Count)][size_t(TargetKind::Count)][size_t(BlendState::Count)][size_t(StencilState::Count)] =
		{};
	SDL_GPUShader* vertex_shaders[3] = {};
	SDL_GPUShader* fragment_shaders[8] = {};
	SDL_GPUSampler* linear_sampler = nullptr;
	SDL_GPUSampler* clamp_sampler = nullptr;
	GeometryView* fullscreen_quad = nullptr;

	// Recording, between BeginFrame() and EndFrame().
	bool in_frame = false;
	SDL_GPUCommandBuffer* command_buffer = nullptr;
	SDL_GPUTexture* target_texture = nullptr;
	int width = 0;
	int height = 0;
	SDL_GPUCopyPass* copy_pass = nullptr;
	uint64_t frame_number = 0;
	Rml::Vector<Rml::UniquePtr<RenderTargets>> render_targets;
	RenderTargets* targets = nullptr;
	int num_layers = 0;
	Rml::Matrix4f projection;
	DrawState state = {};
	Rml::Vector<std::function<void()>> commands;

	// Running the commands, in EndFrame().
	SDL_GPURenderPass* render_pass = nullptr;
	SDL_GPUTexture* pass_texture = nullptr;
	int postprocess_order[4] = {0, 1, 2, 3};     // primary, secondary, tertiary, blend mask

	Rml::Vector<Rml::UniquePtr<Buffer>> buffers; // sorted by capacity

	SDL_GPUGraphicsPipeline* GetPipeline(ProgramId program, TargetKind kind, BlendState blend, StencilState stencil);
	RenderTargets* GetRenderTargets(int new_width, int new_height);
	void ReleaseRenderTargets(RenderTargets& render_targets);
	SDL_GPUTexture* GetLayer(int layer);
	SDL_GPUTexture* GetPostprocess(int role);
	void SwapPostprocessPrimarySecondary();

	Buffer* RequestBuffer(int capacity, SDL_GPUBufferUsageFlags usage);
	GeometryView* UploadGeometry(SDL_GPUCopyPass* pass, Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices);
	void ExecuteCommands();

	// Helpers for the commands.
	void EndPass();
	void BeginLayerPass(int layer, bool clear_color, bool clear_stencil);
	void BeginDraw(SDL_GPUTexture* target, TargetKind kind, ProgramId program, BlendState blend, StencilState stencil, Rml::Rectanglei scissor);
	void SetViewport(Rml::Rectanglef viewport);
	void DrawGeometry(const GeometryView* geometry);
	// Draws the fullscreen quad with the bound pipeline, reading the source texture at transformed coordinates.
	void DrawQuad(SDL_GPUTexture* source, Rml::Vector2f uv_scale = Rml::Vector2f(1.f), Rml::Vector2f uv_offset = Rml::Vector2f(0.f));
	// Draws the quad into a region of the bound target (all of it if invalid), reading the source at the same place. On
	// multisampled layers, quads are bounded by their geometry as well as by the scissor: Mesa's lavapipe lets a
	// scissored draw cover half the samples of the pixels left of and above the scissor region.
	void DrawQuadIn(Rml::Rectanglei region, SDL_GPUTexture* source);
	void ResolveLayer(int layer, SDL_GPUTexture* destination);
	void CopyTexture(SDL_GPUTexture* source, Rml::Rectanglei region, SDL_GPUTexture* destination);
	void RenderGeometryWith(ProgramId program, const DrawState& draw_state, const GeometryView* geometry, Rml::Vector2f translation,
		SDL_GPUTexture* texture);
	void RenderBlur(float sigma, SDL_GPUTexture* source_destination, SDL_GPUTexture* temp, Rml::Rectanglei window);
	void RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters, Rml::Rectanglei scissor);
	Rml::Rectanglei ClampToTarget(Rml::Rectanglei region) const;
};
