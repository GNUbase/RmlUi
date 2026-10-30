#include "DecoratorSVG.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "SVGCache.h"

namespace Rml {
namespace SVG {

	// Locates a file named in a decorator from the style sheet that declared it. Without a style sheet, the file is found relative to the
	// element's document.
	static void LocateSource(String& source, SVGCache::SourceType& source_type, const DecoratorInstancerInterface& instancer_interface)
	{
		const String& style_sheet_path = instancer_interface.GetSourcePath();
		source_type = SVGCache::File;
		if (style_sheet_path.empty())
			return;

		String path;
		GetSystemInterface()->JoinPath(path, StringUtilities::Replace(style_sheet_path, '|', ':'), source);
		source = path;
		source_type = SVGCache::ResolvedFile;
	}

	DecoratorSVG::DecoratorSVG(const String& source, const SVGCache::SourceType source_type, const bool crop_to_content) :
		source_path(source), source_type(source_type), crop_to_content(crop_to_content)
	{
		RMLUI_ASSERT(!source_path.empty());
	}

	DecoratorSVG::~DecoratorSVG() {}

	DecoratorDataHandle DecoratorSVG::GenerateElementData(Element* element, BoxArea paint_area) const
	{
		SharedPtr<SVGData> handle = SVGCache::GetHandle(source_path, source_path, source_type, element, crop_to_content, paint_area);
		if (!handle)
			return {};

		Data* data = new Data{
			std::move(handle),
			paint_area,
		};

		return reinterpret_cast<DecoratorDataHandle>(data);
	}

	void DecoratorSVG::ReleaseElementData(DecoratorDataHandle element_data) const
	{
		Data* data = reinterpret_cast<Data*>(element_data);
		delete data;
	}

	void DecoratorSVG::RenderElement(Element* element, DecoratorDataHandle element_data) const
	{
		Data* data = reinterpret_cast<Data*>(element_data);
		RMLUI_ASSERT(data && data->handle);
		data->handle->geometry.Render(element->GetAbsoluteOffset(data->paint_area), data->handle->texture);
	}

	DecoratorSVGInstancer::DecoratorSVGInstancer()
	{
		source_id = RegisterProperty("source", "").AddParser("string").GetId();
		crop_id = RegisterProperty("crop", "crop-none").AddParser("keyword", "crop-none, crop-to-content").GetId();
		RegisterShorthand("decorator", "source, crop", ShorthandType::FallThrough);
	}

	DecoratorSVGInstancer::~DecoratorSVGInstancer() {}

	SharedPtr<Decorator> DecoratorSVGInstancer::InstanceDecorator(const String&, const PropertyDictionary& properties,
		const DecoratorInstancerInterface& instancer_interface)
	{
		String source = properties.GetProperty(source_id)->Get<String>();
		if (source.empty())
			return nullptr;

		SVGCache::SourceType source_type;
		LocateSource(source, source_type, instancer_interface);

		const bool crop_to_content = properties.GetProperty(crop_id)->Get<int>() != 0;

		return MakeShared<DecoratorSVG>(source, source_type, crop_to_content);
	}

	struct DecoratorSVGNinePatch::Data {
		SharedPtr<SVGData> handle;
		Geometry geometry;
	};

	DecoratorSVGNinePatch::DecoratorSVGNinePatch(const String& source, const SVGCache::SourceType source_type, const Array<float, 4>& slice) :
		source_path(source), source_type(source_type), slice(slice)
	{
		RMLUI_ASSERT(!source_path.empty());
	}

	DecoratorSVGNinePatch::~DecoratorSVGNinePatch() {}

	DecoratorDataHandle DecoratorSVGNinePatch::GenerateElementData(Element* element, BoxArea paint_area) const
	{
		String path = source_path;
		if (source_type == SVGCache::File)
		{
			if (ElementDocument* document = element->GetOwnerDocument())
				GetSystemInterface()->JoinPath(path, StringUtilities::Replace(document->GetSourceURL(), '|', ':'), source_path);
		}

		const float scale = ElementUtilities::GetDensityIndependentPixelRatio(element);
		SharedPtr<SVGData> handle = SVGCache::GetHandle(path, element, scale);
		if (!handle)
			return {};

		const Vector2f texture_dimensions(SVGCache::GetScaledDimensions(handle->intrinsic_dimensions, scale));
		if (texture_dimensions.x <= 0.f || texture_dimensions.y <= 0.f)
			return {};

		const ComputedValues& computed = element->GetComputedValues();
		const ColourbPremultiplied quad_colour = computed.image_color().ToPremultiplied(computed.opacity());

		const RenderBox render_box = element->GetRenderBox(paint_area);
		const Vector2f surface_offset = render_box.GetFillOffset();
		const Vector2f surface_dimensions = render_box.GetFillSize();

		// The slice lines in texture pixels, rounded so that each patch covers whole pixels of the raster. The corners are drawn at this
		// size.
		const Vector2f top_left = Vector2f(slice[3], slice[0]) * scale;
		const Vector2f bottom_right = Vector2f(slice[1], slice[2]) * scale;
		const Vector2f tex_pos[4] = {
			Vector2f(0.f),
			top_left.Round(),
			texture_dimensions - bottom_right.Round(),
			texture_dimensions,
		};

		Vector2f tex_coords[4];
		for (int i = 0; i < 4; i++)
			tex_coords[i] = tex_pos[i] / texture_dimensions;

		// Surface positions in pixels: the corners keep their size, the edges and the center stretch.
		Vector2f surface_pos[4] = {
			Vector2f(0.f),
			tex_pos[1] - tex_pos[0],
			surface_dimensions - (tex_pos[3] - tex_pos[2]),
			surface_dimensions,
		};

		// When the surface is smaller than the corners, scale them down, one dimension at a time.
		const Vector2f surface_center_size = surface_pos[2] - surface_pos[1];
		for (int i = 0; i < 2; i++)
		{
			if (surface_center_size[i] < 0.f)
			{
				const float top_left_size = tex_pos[1][i] - tex_pos[0][i];
				const float bottom_right_size = tex_pos[3][i] - tex_pos[2][i];
				surface_pos[1][i] = top_left_size / (top_left_size + bottom_right_size) * surface_dimensions[i];
				surface_pos[2][i] = surface_pos[1][i];
			}
		}

		for (Vector2f& position : surface_pos)
			position += surface_offset;

		surface_pos[1] = surface_pos[1].Round();
		surface_pos[2] = surface_pos[2].Round();

		// Expand the four diagonal grid points to the sixteen vertices of the nine patches.
		Mesh mesh;
		mesh.vertices.resize(4 * 4);
		for (int y = 0; y < 4; y++)
		{
			for (int x = 0; x < 4; x++)
			{
				Vertex& vertex = mesh.vertices[y * 4 + x];
				vertex.colour = quad_colour;
				vertex.position = {surface_pos[x].x, surface_pos[y].y};
				vertex.tex_coord = {tex_coords[x].x, tex_coords[y].y};
			}
		}

		// Two triangles for each of the nine patches.
		mesh.indices.resize(9 * 2 * 3);
		const int top_left_indices[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
		for (int patch = 0; patch < 9; patch++)
		{
			const int i = patch * 6;
			const int top_left_index = top_left_indices[patch];
			mesh.indices[i] = top_left_index;
			mesh.indices[i + 1] = top_left_index + 4;
			mesh.indices[i + 2] = top_left_index + 1;
			mesh.indices[i + 3] = top_left_index + 1;
			mesh.indices[i + 4] = top_left_index + 4;
			mesh.indices[i + 5] = top_left_index + 5;
		}

		Data* data = new Data{std::move(handle), element->GetRenderManager()->MakeGeometry(std::move(mesh))};
		return reinterpret_cast<DecoratorDataHandle>(data);
	}

	void DecoratorSVGNinePatch::ReleaseElementData(DecoratorDataHandle element_data) const
	{
		delete reinterpret_cast<Data*>(element_data);
	}

	void DecoratorSVGNinePatch::RenderElement(Element* element, DecoratorDataHandle element_data) const
	{
		Data* data = reinterpret_cast<Data*>(element_data);
		RMLUI_ASSERT(data && data->handle);
		data->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border), data->handle->texture);
	}

	DecoratorSVGNinePatchInstancer::DecoratorSVGNinePatchInstancer()
	{
		source_id = RegisterProperty("source", "").AddParser("string").GetId();
		slice_ids[0] = RegisterProperty("slice-top", "0").AddParser("number").GetId();
		slice_ids[1] = RegisterProperty("slice-right", "0").AddParser("number").GetId();
		slice_ids[2] = RegisterProperty("slice-bottom", "0").AddParser("number").GetId();
		slice_ids[3] = RegisterProperty("slice-left", "0").AddParser("number").GetId();
		RegisterShorthand("slice", "slice-top, slice-right, slice-bottom, slice-left", ShorthandType::Box);
		RegisterShorthand("decorator", "source, slice", ShorthandType::RecursiveCommaSeparated);
	}

	DecoratorSVGNinePatchInstancer::~DecoratorSVGNinePatchInstancer() {}

	SharedPtr<Decorator> DecoratorSVGNinePatchInstancer::InstanceDecorator(const String&, const PropertyDictionary& properties,
		const DecoratorInstancerInterface& instancer_interface)
	{
		String source = properties.GetProperty(source_id)->Get<String>();
		if (source.empty())
			return nullptr;

		SVGCache::SourceType source_type;
		LocateSource(source, source_type, instancer_interface);

		Array<float, 4> slice;
		for (int i = 0; i < 4; i++)
			slice[i] = Math::Max(properties.GetProperty(slice_ids[i])->Get<float>(), 0.f);

		return MakeShared<DecoratorSVGNinePatch>(source, source_type, slice);
	}

} // namespace SVG
} // namespace Rml
