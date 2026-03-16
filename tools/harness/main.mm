#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "plugin/looper_plugin.h"

// --- OpenGL View ---

@interface HarnessGLView : NSOpenGLView {
  LooperPlugin* _plugin;
  GLuint _inputTex;
  BOOL _glReady;
  BOOL _synthEnabled;
}
@end

@implementation HarnessGLView

- (instancetype)initWithFrame:(NSRect)frame {
  NSOpenGLPixelFormatAttribute attrs[] = {
    NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
    NSOpenGLPFAColorSize, 24,
    NSOpenGLPFAAlphaSize, 8,
    NSOpenGLPFADoubleBuffer,
    NSOpenGLPFAAccelerated,
    0
  };
  NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
  self = [super initWithFrame:frame pixelFormat:pf];
  if (self) {
    [self setWantsBestResolutionOpenGLSurface:YES];
  }
  return self;
}

- (void)prepareOpenGL {
  [super prepareOpenGL];

  GLint swapInt = 1;
  [[self openGLContext] setValues:&swapInt forParameter:NSOpenGLContextParameterSwapInterval];

  // Create a simple input texture (dark gradient)
  glGenTextures(1, &_inputTex);
  glBindTexture(GL_TEXTURE_2D, _inputTex);
  int tw = 256, th = 256;
  std::vector<uint8_t> pixels(tw * th * 4);
  for (int y = 0; y < th; ++y) {
    for (int x = 0; x < tw; ++x) {
      int i = (y * tw + x) * 4;
      pixels[i + 0] = (uint8_t)(x * 40 / tw);
      pixels[i + 1] = (uint8_t)(20 + y * 30 / th);
      pixels[i + 2] = (uint8_t)(40 + x * 20 / tw);
      pixels[i + 3] = 255;
    }
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  _plugin = new LooperPlugin();
  NSRect backing = [self convertRectToBacking:[self bounds]];
  FFGLViewportStruct vp = {};
  vp.width = (GLuint)backing.size.width;
  vp.height = (GLuint)backing.size.height;
  _plugin->InitGL(&vp);
  _glReady = YES;

  NSTimer* timer = [NSTimer timerWithTimeInterval:1.0/60.0
                                           target:self
                                         selector:@selector(timerFired:)
                                         userInfo:nil
                                          repeats:YES];
  [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
}

- (void)timerFired:(NSTimer*)timer {
  [self setNeedsDisplay:YES];
}

- (void)reshape {
  [super reshape];
  if (!_glReady) return;
  [[self openGLContext] makeCurrentContext];
  NSRect backing = [self convertRectToBacking:[self bounds]];
  FFGLViewportStruct vp = {};
  vp.width = (GLuint)backing.size.width;
  vp.height = (GLuint)backing.size.height;
  _plugin->Resize(&vp);
}

- (void)drawRect:(NSRect)dirtyRect {
  if (!_glReady) return;
  [[self openGLContext] makeCurrentContext];

  FFGLTextureStruct tex = {};
  tex.Width = 256;
  tex.Height = 256;
  tex.HardwareWidth = 256;
  tex.HardwareHeight = 256;
  tex.Handle = _inputTex;

  FFGLTextureStruct* texPtr = &tex;
  ProcessOpenGLStruct processStruct = {};
  processStruct.numInputTextures = 1;
  processStruct.inputTextures = &texPtr;
  processStruct.HostFBO = 0;

  _plugin->ProcessOpenGL(&processStruct);

  [[self openGLContext] flushBuffer];
}

- (BOOL)acceptsFirstResponder { return YES; }

// Map character to plugin param ID. Returns -1 if not mapped.
static int paramForChar(unichar c) {
  switch (c) {
    case '1': return PID_TRIGGER_1;
    case '2': return PID_TRIGGER_2;
    case '3': return PID_TRIGGER_3;
    case '4': return PID_TRIGGER_4;
    case 'd': return PID_DELETE;
    case 'm': return PID_MUTE;
    case 'z': return PID_UNDO;
    case 'x': return PID_REDO;
    case 'r': return PID_RECORD;
    default:  return -1;
  }
}

- (void)keyDown:(NSEvent *)event {
  if ([event isARepeat]) return;

  NSString* chars = [event charactersIgnoringModifiers];
  if ([chars length] == 0) return;
  unichar c = [chars characterAtIndex:0];

  if (c == 'q' || c == 27) {
    [[NSApplication sharedApplication] terminate:nil];
    return;
  }

  // Synth toggle
  if (c == 's') {
    _synthEnabled = !_synthEnabled;
    _plugin->SetFloatParameter(PID_SYNTH, _synthEnabled ? 1.0f : 0.0f);
    return;
  }

  // All params are boolean / piano-key: keyDown → 1.0
  int pid = paramForChar(c);
  if (pid >= 0)
    _plugin->SetFloatParameter(pid, 1.0f);
}

- (void)keyUp:(NSEvent *)event {
  NSString* chars = [event charactersIgnoringModifiers];
  if ([chars length] == 0) return;
  unichar c = [chars characterAtIndex:0];

  // keyUp → 0.0
  int pid = paramForChar(c);
  if (pid >= 0)
    _plugin->SetFloatParameter(pid, 0.0f);
}

- (void)dealloc {
  if (_plugin) {
    _plugin->DeInitGL();
    delete _plugin;
  }
  if (_inputTex) glDeleteTextures(1, &_inputTex);
}

@end

// --- App Delegate ---

@interface HarnessAppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow* window;
@end

@implementation HarnessAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  NSRect frame = NSMakeRect(100, 100, 640, 480);
  self.window = [[NSWindow alloc]
    initWithContentRect:frame
    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
    backing:NSBackingStoreBuffered
    defer:NO];
  [self.window setTitle:@"Looper Harness"];
  [self.window setMinSize:NSMakeSize(400, 300)];

  HarnessGLView* glView = [[HarnessGLView alloc] initWithFrame:frame];
  [self.window setContentView:glView];
  [self.window makeFirstResponder:glView];
  [self.window makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  return YES;
}

@end

// --- Main ---

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];

    HarnessAppDelegate* delegate = [[HarnessAppDelegate alloc] init];
    [app setDelegate:delegate];
    [app activateIgnoringOtherApps:YES];
    [app run];
  }
  return 0;
}
