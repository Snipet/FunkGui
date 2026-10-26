#include "ShaderLibraryDetail.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// The Metal half of shaders::set()'s checks (v0.19.0; include/funkgui/gpu/ShaderLibrary.h). bgfx compiles a shader's
// MSL text at the next bgfx::frame() and aborts the process when Metal's compiler refuses it (renderer_mtl.cpp
// ShaderMtl::create: BGFX_FATAL InvalidShader), so set() compiles the same text the same way first, on the system's
// default device: [MTLDevice newLibraryWithSource:options:nil error:] on the text read as an ASCII C string (bgfx's
// NS::String::string(code, NS::ASCIIStringEncoding)), then the entry point bgfx asks for. A refusal here is a false
// from set(), with Metal's log, instead of an abort later.
//
// Compiled without ARC, like NativeSurface.mm: what the Create and new methods return is released here.

namespace funkgui::shaders::detail
{
    bool metalCompiles(const std::string& msl, std::string& why)
    {
        @autoreleasepool
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil)
            {
                why = "no Metal device to compile the shader on";
                return false;
            }
            NSString* source = [NSString stringWithCString:msl.c_str() encoding:NSASCIIStringEncoding];
            if (source == nil)
            {
                [device release];
                why = "the shader's Metal code is not ASCII text (bgfx reads it as ASCII)";
                return false;
            }
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (library == nil)
            {
                const char* log = error != nil ? [[error localizedDescription] UTF8String] : nullptr;
                why = std::string("Metal did not compile the shader: ") + (log != nullptr ? log : "no log");
                [device release];
                return false;
            }
            id<MTLFunction> entry = [library newFunctionWithName:@"xlatMtlMain"];
            const bool ok = entry != nil;
            if (!ok)
                why = "the shader's Metal code has no xlatMtlMain (not shaderc's metal output)";
            [entry release];
            [library release];
            [device release];
            return ok;
        }
    }
}
