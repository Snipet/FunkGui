#import <Cocoa/Cocoa.h>

#include "Cursors.h"

#include <cmath>

// The closed hand and the not-allowed sign on macOS (Cursors.h; FSynth G1): AppKit's own cursors, turned into
// juce::MouseCursors, because a juce::Component can only be given a cursor JUCE made. Each is drawn at 2x into a
// juce::Image (the backing scale of every current Mac display; JUCE hands NSCursor the image with its scale, and
// AppKit scales it down for a 1x display) and keeps the NSCursor's hot spot, which AppKit gives in the image's
// points, the unit JUCE's custom cursors take theirs in.
//
// Compiled without ARC, like NativeSurface.mm: everything AppKit returns here is autoreleased or owned by its image,
// and the Core Graphics objects made here are released here.
//
// A cursor AppKit cannot give an image for falls back to the standard cursor Linux uses (linux/Cursors.cpp), so the
// pointer still changes on press; fg.editorhost.cursor then fails its "equals no standard type" rows, which is how
// such a system would be noticed.

namespace funkgui
{
    namespace
    {
        constexpr float kImageScale = 2.0f;              // image px per point
        constexpr int   kMaxSidePx = 512;                // a cursor image is tens of px: anything larger is not one

        juce::MouseCursor fromSystemCursor(NSCursor* cursor, juce::MouseCursor::StandardCursorType fallback)
        {
            @autoreleasepool
            {
                NSImage* image = cursor != nil ? [cursor image] : nil;
                if (image == nil)
                    return fallback;
                const NSSize points = [image size];
                const int w = static_cast<int>(std::lround(points.width * static_cast<double>(kImageScale)));
                const int h = static_cast<int>(std::lround(points.height * static_cast<double>(kImageScale)));
                if (w <= 0 || h <= 0 || w > kMaxSidePx || h > kMaxSidePx)
                    return fallback;

                // The image's representation for a 2x context (JUCE's own way, juce_MouseCursor_mac.mm:88-104).
                NSAffineTransform* scale = [NSAffineTransform transform];
                [scale scaleBy: static_cast<CGFloat>(kImageScale)];
                CGImageRef pixels = [image CGImageForProposedRect: nil
                                                          context: nil
                                                            hints: [NSDictionary dictionaryWithObject: scale
                                                                                               forKey: NSImageHintCTM]];
                if (pixels == nullptr)
                    return fallback;

                // juce::Image::ARGB is premultiplied BGRA in memory on a little-endian machine, which is this bitmap
                // context's layout; its first row is the image's top row, as JUCE's is.
                juce::Image drawn(juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
                {
                    juce::Image::BitmapData bits(drawn, juce::Image::BitmapData::writeOnly);
                    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                    CGContextRef context = CGBitmapContextCreate(
                        bits.data, static_cast<size_t>(w), static_cast<size_t>(h), 8, static_cast<size_t>(bits.lineStride),
                        space,
                        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst)
                            | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little));
                    CGColorSpaceRelease(space);
                    if (context == nullptr)
                        return fallback;
                    const CGRect all = CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h));
                    CGContextClearRect(context, all);
                    CGContextDrawImage(context, all, pixels);
                    CGContextRelease(context);
                }

                // The hot spot stays inside the image, which JUCE requires of a custom cursor.
                const NSPoint hot = [cursor hotSpot];
                const int hotX = juce::jlimit(0, juce::jmax(0, juce::roundToInt(points.width) - 1),
                                              juce::roundToInt(hot.x));
                const int hotY = juce::jlimit(0, juce::jmax(0, juce::roundToInt(points.height) - 1),
                                              juce::roundToInt(hot.y));
                return juce::MouseCursor(drawn, hotX, hotY, kImageScale);
            }
        }
    }

    juce::MouseCursor makeGrabbingCursor()
    {
        return fromSystemCursor([NSCursor closedHandCursor], juce::MouseCursor::DraggingHandCursor);
    }

    juce::MouseCursor makeNotAllowedCursor()
    {
        return fromSystemCursor([NSCursor operationNotAllowedCursor], juce::MouseCursor::NormalCursor);
    }
}
