#pragma once

#include "../../Include/RmlUi/Core/Decorator.h"
#include "SVGCache.h"

namespace Rml {
namespace SVG {

	struct SVGData;

	class DecoratorSVG : public Decorator {
	public:
		DecoratorSVG(const String& source, SVGCache::SourceType source_type, const bool crop_to_content);
		virtual ~DecoratorSVG();

		DecoratorDataHandle GenerateElementData(Element* element, BoxArea paint_area) const override;
		void ReleaseElementData(DecoratorDataHandle element_data) const override;

		void RenderElement(Element* element, DecoratorDataHandle element_data) const override;

	private:
		struct Data {
			SharedPtr<SVGData> handle;
			BoxArea paint_area;
		};

		String source_path;
		SVGCache::SourceType source_type;
		bool crop_to_content;
	};

	class DecoratorSVGInstancer : public DecoratorInstancer {
	public:
		DecoratorSVGInstancer();
		~DecoratorSVGInstancer();

		SharedPtr<Decorator> InstanceDecorator(const String&, const PropertyDictionary& properties, const DecoratorInstancerInterface&) override;

	private:
		PropertyId source_id;
		PropertyId crop_id;
	};

	/// Draws an SVG file as a nine-patch: the image is rasterized at its intrinsic size, scaled by the density-independent pixel ratio, and
	/// divided by the four slice lines. The corners keep their size, the edges stretch along one axis and the center along both, as with
	/// 'border-image' in CSS. Every element drawing the same file at the same ratio shares one raster, whatever its size.
	class DecoratorSVGNinePatch : public Decorator {
	public:
		/// @param[in] source Path to the SVG file.
		/// @param[in] source_type Whether the path is relative to the element's document (File) or used as it is (ResolvedFile).
		/// @param[in] slice Distances of the slice lines from the top, right, bottom and left edges, in the SVG's units.
		DecoratorSVGNinePatch(const String& source, SVGCache::SourceType source_type, const Array<float, 4>& slice);
		virtual ~DecoratorSVGNinePatch();

		DecoratorDataHandle GenerateElementData(Element* element, BoxArea paint_area) const override;
		void ReleaseElementData(DecoratorDataHandle element_data) const override;

		void RenderElement(Element* element, DecoratorDataHandle element_data) const override;

	private:
		struct Data;

		String source_path;
		SVGCache::SourceType source_type;
		Array<float, 4> slice;
	};

	/// The decorator 'svg-ninepatch(<source>, <slice>)', where slice is one to four numbers in the SVG's units, as for 'padding'.
	class DecoratorSVGNinePatchInstancer : public DecoratorInstancer {
	public:
		DecoratorSVGNinePatchInstancer();
		~DecoratorSVGNinePatchInstancer();

		SharedPtr<Decorator> InstanceDecorator(const String&, const PropertyDictionary& properties, const DecoratorInstancerInterface&) override;

	private:
		PropertyId source_id;
		PropertyId slice_ids[4];
	};

} // namespace SVG
} // namespace Rml
