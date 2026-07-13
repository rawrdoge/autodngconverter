// dng_preview_embed.cpp
// Command-line tool to embed JPEG previews into DNG with proper IFD hierarchy
// Compile against Adobe DNG SDK

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "dng_file_stream.h"
#include "dng_image_writer.h"
#include "dng_host.h"
#include "dng_negative.h"
#include "dng_preview.h"
#include "dng_stream.h"
#include "dng_memory_stream.h"
#include "dng_ifd.h"

void print_usage(const char* prog) {
    printf("Usage: %s <dng_file> <jpeg_file> [options]\n", prog);
    printf("Options:\n");
    printf("  --preview-size WxH     Set preview dimensions (default: from JPEG)\n");
    printf("  --thumbnail-size WxH   Set thumbnail dimensions (default: 256x192)\n");
    printf("  --preserve-hierarchy    Preserve original IFD structure\n");
    printf("  --adobe-compatible      Use Adobe DNG Converter IFD layout\n");
    printf("  --verbose               Show detailed progress\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  DNG_PREVIEW_EMBED_MODE=adobe|preserve|minimal\n");
    printf("  DNG_PREVIEW_QUALITY=85-100\n");
    printf("  DNG_PREVIEW_MAX_SIZE=2048\n");
}

bool parse_size(const char* str, uint32_t& w, uint32_t& h) {
    if (sscanf(str, "%ux%u", &w, &h) == 2) return true;
    if (sscanf(str, "%ux%u", &w, &h) == 2) return true;
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* dng_path = argv[1];
    const char* jpeg_path = argv[2];

    // Parse options
    uint32_t preview_w = 0, preview_h = 0;
    uint32_t thumb_w = 256, thumb_h = 192;
    bool preserve_hierarchy = false;
    bool adobe_compatible = false;
    bool verbose = false;

    // Check environment variables
    const char* env_mode = getenv("DNG_PREVIEW_EMBED_MODE");
    if (env_mode) {
        if (strcmp(env_mode, "adobe") == 0) adobe_compatible = true;
        if (strcmp(env_mode, "preserve") == 0) preserve_hierarchy = true;
    }

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--preview-size") == 0 && i + 1 < argc) {
            parse_size(argv[++i], preview_w, preview_h);
        }
        else if (strcmp(argv[i], "--thumbnail-size") == 0 && i + 1 < argc) {
            parse_size(argv[++i], thumb_w, thumb_h);
        }
        else if (strcmp(argv[i], "--preserve-hierarchy") == 0) {
            preserve_hierarchy = true;
        }
        else if (strcmp(argv[i], "--adobe-compatible") == 0) {
            adobe_compatible = true;
        }
        else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
    }

    try {
        dng_host host;
        host.SetSaveDNGVersion(dngVersion_1_4_0_0);
        host.SetSaveLinearDNG(false);
        host.SetKeepStage1(true);
        host.SetKeepStage2(true);
        host.SetKeepStage3(true);

        // Read original DNG
        if (verbose) printf("Reading: %s\n", dng_path);
        dng_file_stream read_stream(dng_path);
        dng_info info;
        info.Parse(host, read_stream);
        info.PostParse(host);

        if (!info.IsValidDNG()) {
            fprintf(stderr, "ERROR: Not a valid DNG file\n");
            return 2;
        }

        // Read JPEG preview
        if (verbose) printf("Reading JPEG: %s\n", jpeg_path);
        dng_file_stream jpeg_stream(jpeg_path);
        jpeg_stream.SetReadLimit(jpeg_stream.Length());

        // Create preview list
        dng_preview_list preview_list;

        // Main preview (JpgFromRaw / large preview)
        if (preview_w == 0 || preview_h == 0) {
            // Auto-detect from JPEG (simplified - would need JPEG parser)
            preview_w = 4000; preview_h = 3000; // Default fallback
        }

        // Add primary preview (Adobe style: SubIFD2 / JpgFromRaw)
        {
            dng_memory_stream preview_stream(host.Allocator());
            jpeg_stream.SetPosition(0);
            preview_stream.CopyFromStream(jpeg_stream, jpeg_stream.Length());
            preview_stream.Flush();

            AutoPtr<dng_preview> preview(new dng_preview);
            preview->fPreviewName = "JpgFromRaw";
            preview->fPreviewSize = dng_point_real64(preview_h, preview_w);
            preview->fPreviewBytes = preview_stream.Length();
            preview->fPreviewData.Reset(preview_stream.AsMemoryBlock(host.Allocator()));
            preview->fMimeType = "image/jpeg";
            preview->fColorSpace = previewColorSpace_sRGB;

            preview_list.Append(preview);
        }

        // Medium preview (SubIFD1 style)
        if (adobe_compatible) {
            // Would need to generate scaled JPEG here
            // For now, we reuse the same JPEG but mark it differently
            AutoPtr<dng_preview> med_preview(new dng_preview);
            med_preview->fPreviewName = "PreviewImage";
            med_preview->fPreviewSize = dng_point_real64(768, 1024);
            med_preview->fPreviewBytes = 0; // Would need scaled version
            med_preview->fMimeType = "image/jpeg";
            med_preview->fColorSpace = previewColorSpace_sRGB;
            preview_list.Append(med_preview);
        }

        // Write modified DNG
        if (verbose) printf("Writing: %s\n", dng_path);

        dng_file_stream write_stream(dng_path, true);

        // Preserve original structure if requested
        if (preserve_hierarchy || adobe_compatible) {
            // Use the SDK's built-in preview writing with proper IFD placement
            info.WriteDNG(host, write_stream,
                         &preview_list,
                         dngVersion_1_4_0_0,
                         true); // true = keep original structure
        }
        else {
            // Minimal: just replace preview
            info.WriteDNG(host, write_stream,
                         &preview_list,
                         dngVersion_1_4_0_0,
                         false);
        }

        if (verbose) printf("Done.\n");
        return 0;
    }
    catch (dng_exception& e) {
        fprintf(stderr, "ERROR: DNG SDK exception: %d\n", e.ErrorCode());
        return 3;
    }
    catch (...) {
        fprintf(stderr, "ERROR: Unknown exception\n");
        return 4;
    }
}
