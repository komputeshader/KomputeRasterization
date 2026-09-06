#include "Application.h"

#include "ForwardRenderer.h"
#include "Settings.h"
#include "Utils.h"

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

namespace
{
	unsigned char VirtualKey(NSEvent* event)
	{
		switch (event.keyCode)
		{
		case 49:
			return 0x20;
		case 123:
			return 0x25;
		case 124:
			return 0x27;
		case 125:
			return 0x28;
		case 126:
			return 0x26;
		}

		NSString* characters = event.charactersIgnoringModifiers.uppercaseString;
		if (characters.length)
		{
			const unichar character = [characters characterAtIndex:0];
			if (character <= 0xff)
			{
				return static_cast<unsigned char>(character);
			}
		}

		return 0;
	}
}

@interface RendererDelegate : NSObject <MTKViewDelegate>
{
	std::unique_ptr<ForwardRenderer> _renderer;
}

- (ForwardRenderer*)renderer;
@end

@implementation RendererDelegate

- (ForwardRenderer*)renderer
{
	return _renderer.get();
}

- (instancetype)initWithView:(MTKView*)view
{
	self = [super init];
	if (self)
	{
		_renderer = std::make_unique<ForwardRenderer>();
		if (!_renderer->Initialize(view))
		{
			return nil;
		}
	}

	return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
	_renderer->DrawableSizeChanged(size);
}

- (void)drawInMTKView:(MTKView*)view
{
	_renderer->Draw(view);
}

@end

@interface RenderView : MTKView
{
	BOOL _trackingThreeFingerPan;
	NSPoint _threeFingerCentroid;
}

@property(nonatomic, weak) RendererDelegate* rendererDelegate;
@end

@implementation RenderView

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)getThreeFingerCentroid:(NSPoint*)centroid event:(NSEvent*)event
{
	NSSet<NSTouch*>* touches = [event touchesMatchingPhase:NSTouchPhaseTouching inView:self];
	if (touches.count != 3)
	{
		return NO;
	}

	NSPoint result = NSZeroPoint;
	for (NSTouch* touch in touches)
	{
		result.x += touch.normalizedPosition.x * touch.deviceSize.width;
		result.y += touch.normalizedPosition.y * touch.deviceSize.height;
	}

	centroid->x = result.x / 3.0;
	centroid->y = result.y / 3.0;
	return YES;
}

- (void)keyDown:(NSEvent*)event
{
	const unsigned char key = VirtualKey(event);
	if (key)
	{
		[self.rendererDelegate renderer]->SetKey(key, true);
		[self.rendererDelegate renderer]->KeyPressed(key);
	}
}

- (void)keyUp:(NSEvent*)event
{
	const unsigned char key = VirtualKey(event);
	if (key)
	{
		[self.rendererDelegate renderer]->SetKey(key, false);
	}
}

- (void)flagsChanged:(NSEvent*)event
{
	if (event.keyCode == 56 || event.keyCode == 60)
	{
		const bool pressed = (event.modifierFlags & NSEventModifierFlagShift) != 0;
		[self.rendererDelegate renderer]->SetKey(0x10, pressed);
	}
}

- (void)rightMouseDragged:(NSEvent*)event
{
	[self.rendererDelegate renderer]->MouseDelta(event.deltaX, event.deltaY);
}

- (void)touchesBeganWithEvent:(NSEvent*)event
{
	_trackingThreeFingerPan = [self getThreeFingerCentroid:&_threeFingerCentroid event:event];
	[super touchesBeganWithEvent:event];
}

- (void)touchesMovedWithEvent:(NSEvent*)event
{
	NSPoint centroid;
	if ([self getThreeFingerCentroid:&centroid event:event])
	{
		if (_trackingThreeFingerPan)
		{
			[self.rendererDelegate renderer]->MouseDelta(
				centroid.x - _threeFingerCentroid.x,
				centroid.y - _threeFingerCentroid.y);
		}

		_threeFingerCentroid = centroid;
		_trackingThreeFingerPan = YES;
	}
	else
	{
		_trackingThreeFingerPan = NO;
	}

	[super touchesMovedWithEvent:event];
}

- (void)touchesEndedWithEvent:(NSEvent*)event
{
	_trackingThreeFingerPan = NO;
	[super touchesEndedWithEvent:event];
}

- (void)touchesCancelledWithEvent:(NSEvent*)event
{
	_trackingThreeFingerPan = NO;
	[super touchesCancelledWithEvent:event];
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) RendererDelegate* rendererDelegate;
@end

@implementation AppDelegate

- (BOOL)applicationShouldRestoreApplicationState:(NSApplication*)sender
{
	(void)sender;
	return NO;
}

- (BOOL)applicationShouldSaveApplicationState:(NSApplication*)sender
{
	(void)sender;
	return NO;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
	(void)notification;
	const CGFloat backingScale = NSScreen.mainScreen.backingScaleFactor;
	const NSRect frame = NSMakeRect(
		0,
		0,
		Settings::BackBufferWidth / backingScale,
		Settings::BackBufferHeight / backingScale);
	self.window = [[NSWindow alloc]
		initWithContentRect:frame
			styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
				NSWindowStyleMaskMiniaturizable
			backing:NSBackingStoreBuffered
			defer:NO];
	self.window.restorable = NO;
	self.window.title = @"KomputeRasterization";
	RenderView* view = [[RenderView alloc] initWithFrame:frame device:MTLCreateSystemDefaultDevice()];
	view.allowedTouchTypes = NSTouchTypeMaskIndirect;
	view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	view.autoResizeDrawable = NO;
	view.drawableSize = CGSizeMake(Settings::BackBufferWidth, Settings::BackBufferHeight);
	view.preferredFramesPerSecond = NSScreen.mainScreen.maximumFramesPerSecond;
	view.paused = NO;
	view.enableSetNeedsDisplay = NO;
	self.rendererDelegate = [[RendererDelegate alloc] initWithView:view];
	if (!self.rendererDelegate)
	{
		[NSApp terminate:nil];
		return;
	}

	view.rendererDelegate = self.rendererDelegate;
	view.delegate = self.rendererDelegate;
	self.window.contentView = view;
	[self.window center];
	[self.window makeKeyAndOrderFront:nil];
	[self.window makeFirstResponder:view];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
	(void)sender;
	return YES;
}

@end

int Application::Run(int argc, const char* argv[])
{
	(void)argc;
	(void)argv;
	@autoreleasepool
	{
		NSApplication* application = [NSApplication sharedApplication];
		application.activationPolicy = NSApplicationActivationPolicyRegular;
		AppDelegate* delegate = [AppDelegate new];
		application.delegate = delegate;
		[application run];
	}

	return 0;
}
