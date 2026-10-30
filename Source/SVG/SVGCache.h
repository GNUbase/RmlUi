#pragma once

#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class Geometry;

namespace SVG {

	struct SVGKey;

	struct SVGData : NonCopyMoveable {
		SVGData(Geometry& geometry, Texture texture, Vector2f intrinsic_dimensions, const SVGKey& cache_key);
		~SVGData();

		Geometry& geometry;
		Texture texture;
		Vector2f intrinsic_dimensions;
		const SVGKey& cache_key;
	};

	class SVGCache {
	public:
		enum SourceType {
			File = 1,         /// The source is a file path, relative to the element's document.
			Data = 2,         /// The source is raw SVG data.
			ResolvedFile = 3, /// The source is a file path that is used as it is, e.g. one already located from a style sheet.
		};
		static void Initialize();
		static void Shutdown();

		/// Returns a handle to SVG data matching the parameters - creates new data if none is found.
		/// @param[in] source_id Key to be used for caching the SVG data.
		/// @param[in] source SVG source data or Path to a file containing the SVG source data (type specified by source_type).
		/// @param[in] source_type If source refers to a file or is the SVG source data itself.
		/// @param[in] element Element for which to calculate the dimensions and color.
		/// @param[in] crop_to_content Crop the rendered SVG to its contents.
		/// @param[in] area The area of the element used to determine the SVG dimensions.
		/// @return A handle to the SVG data, with automatic reference counting.
		///	@note When changing color or dimensions of an SVG without changing the source file, it's best to get a
		/// new handle before releasing the old one, to avoid unnecessarily reloading data.
		static SharedPtr<SVGData> GetHandle(const String& source_id, const String& source, SourceType source_type, Element* element,
			const bool crop_to_content, const BoxArea area);

		/// Returns a handle to the SVG file rasterized at its intrinsic dimensions times the given scale, in white, with geometry of
		/// that size. Every element using the same file at the same scale shares the raster.
		/// @param[in] path Path to the file, used as it is.
		/// @param[in] element Element whose render manager makes the texture.
		/// @param[in] scale Pixels per unit of the SVG's intrinsic dimensions, usually the density-independent pixel ratio.
		static SharedPtr<SVGData> GetHandle(const String& path, Element* element, float scale);

		/// The pixel dimensions of an SVG with the given intrinsic dimensions rasterized at the given scale.
		static Vector2i GetScaledDimensions(Vector2f intrinsic_dimensions, float scale);
	};

} // namespace SVG
} // namespace Rml
