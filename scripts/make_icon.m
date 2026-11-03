#import <AppKit/AppKit.h>
#include <math.h>

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        const int S = 1024;
        const float SQ_SIZE = 824.0f;

        // 1. Load Apple's literal macOS 26 Tahoe `Games.app` icon for exact squircle alpha + rim highlight
        NSBitmapImageRep *stockRep = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:@"/tmp/stock_games_icon.png"]];
        unsigned char *stockBytes = [stockRep bitmapData];

        // 2. Load the full-bleed Liquid Glass controller artwork
        NSImage *srcImg = [[NSImage alloc] initWithContentsOfFile:@"/Users/pascal/.gemini/antigravity/brain/2339c7e0-0879-41fd-98dd-9978a24288ed/devotion_fullbleed_liquid_glass_icon_1793699404491.jpg"];
        NSBitmapImageRep *artRep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:S
                          pixelsHigh:S
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:S * 4
                        bitsPerPixel:32];

        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:artRep]];
        [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];

        [srcImg drawInRect:NSMakeRect(0, 0, S, S)
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationCopy
                  fraction:1.0];
        [srcImg drawInRect:NSMakeRect((S - SQ_SIZE) * 0.5, (S - SQ_SIZE) * 0.5, SQ_SIZE, SQ_SIZE)
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver
                  fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
        unsigned char *artBytes = [artRep bitmapData];

        NSBitmapImageRep *outRep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:S
                          pixelsHigh:S
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:S * 4
                        bitsPerPixel:32];
        unsigned char *outBytes = [outRep bitmapData];

        for (int y = 0; y < S; y++) {
            for (int x = 0; x < S; x++) {
                int idx = (y * S + x) * 4;
                unsigned char st_r = stockBytes[idx + 0];
                unsigned char st_g = stockBytes[idx + 1];
                unsigned char st_b = stockBytes[idx + 2];
                unsigned char st_a = stockBytes[idx + 3];

                // Symmetrized squircle alpha (removes the downward drop shadow to get pure squircle mask)
                unsigned char a1 = stockBytes[(y * S + x) * 4 + 3];
                unsigned char a2 = stockBytes[(y * S + (S - 1 - x)) * 4 + 3];
                unsigned char a3 = stockBytes[((S - 1 - y) * S + x) * 4 + 3];
                unsigned char a4 = stockBytes[((S - 1 - y) * S + (S - 1 - x)) * 4 + 3];
                unsigned char min_a = a1;
                if (a2 < min_a) min_a = a2;
                if (a3 < min_a) min_a = a3;
                if (a4 < min_a) min_a = a4;

                // Pure squircle mask: outside squircle, min_a <= 19 (ambient shadow halo); inside squircle, min_a >= 50..255
                float sq_mask = fminf(1.0f, fmaxf(0.0f, ((float)min_a - 20.0f) / (255.0f - 20.0f)));

                // Extract Apple's exact neutral specular rim highlight from stock `Games.app`:
                // On the outer border of `Games.app`, the pixels are neutral grey (`abs(st_r - st_b) <= 4`) and brighter than the dark base (~25)
                float rim_highlight = 0.0f;
                if (abs((int)st_r - (int)st_b) <= 5 && st_b > 26 && sq_mask > 0.01f) {
                    // Un-premultiply stock grey value if on anti-aliased edge
                    float unprem = (sq_mask > 0.1f) ? ((float)st_b / sq_mask) : (float)st_b;
                    rim_highlight = fmaxf(0.0f, unprem - 26.0f);
                }

                float r = (float)artBytes[idx + 0];
                float g = (float)artBytes[idx + 1];
                float b = (float)artBytes[idx + 2];

                // Apply Apple's exact Liquid Glass rim highlight (screen-blended with warm ruby-white tint)
                float rim_norm = fminf(1.0f, rim_highlight / 200.0f);
                r = r + (255.0f - r) * rim_norm * 0.85f;
                g = g + (230.0f - g) * rim_norm * 0.72f;
                b = b + (235.0f - b) * rim_norm * 0.75f;

                // Premultiply RGB by `sq_mask` (so outside the squircle, RGB is 0,0,0 for the drop shadow, and inside it's our icon!)
                outBytes[idx + 0] = (unsigned char)fminf(255.0f, fmaxf(0.0f, r * sq_mask + 0.5f));
                outBytes[idx + 1] = (unsigned char)fminf(255.0f, fmaxf(0.0f, g * sq_mask + 0.5f));
                outBytes[idx + 2] = (unsigned char)fminf(255.0f, fmaxf(0.0f, b * sq_mask + 0.5f));
                // Use Apple's EXACT alpha channel from `Games.app` (squircle + anti-aliased edge + native drop shadow!)
                outBytes[idx + 3] = st_a;
            }
        }

        NSData *pngData = [outRep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [pngData writeToFile:@"/tmp/candidate_a_exact_mac_squircle.png" atomically:YES];
        printf("Saved /tmp/candidate_a_exact_mac_squircle.png\n");
    }
    return 0;
}
