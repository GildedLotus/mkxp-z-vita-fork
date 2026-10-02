# Test suite for mkxp-z stretch_blt.
# Copyright 2024 Splendide Imaginarius.
# License GPLv2+.
#
# Run the suite via the "customScript" field in mkxp.json.
# Use RGSS v3 for best results.

def dump(bmp, spr, desc)
	spr.bitmap = bmp
	Graphics.wait(1)
	bmp.to_file("test-results/" + desc + ".png")
	System::puts("Finished " + desc)
end

Dir.mkdir("test-results") unless Dir.exist?("test-results")

# Setup graphics
Graphics.resize_screen(640, 480)

# Setup font
fnt = Font.new("Liberation Sans", 32)

# Setup splash screen
bmp = Bitmap.new(640, 480)
bmp.fill_rect(0, 0, 640, 480, Color.new(0, 0, 0))

bmp.font = fnt
bmp.draw_text(0, 0, 640, 240, "stretch_blt Test Suite", 1)
bmp.draw_text(0, 240, 640, 240, "Starting Now", 1)

spr = Sprite.new()
spr.bitmap = bmp

Graphics.wait(1 * 60)

# Tests start here

# The source rectangle below deliberately extends beyond this bitmap.
# Generate the image here so the bounds probe needs no external decoder/asset.
foreground = Bitmap.new(320, 240)
foreground.gradient_fill_rect(Rect.new(0, 0, 320, 240),
                              Color.new(255, 0, 0), Color.new(0, 0, 255))
dump(foreground, spr, "foreground")

background = Bitmap.new(640, 480)
background.clear
dump(background, spr, "background")

composite = background.dup
composite.stretch_blt(Rect.new(0, 0, 640, 480), foreground, Rect.new(0, 0, 640, 480))
dump(composite, spr, "composite")

# Tests are finished, show exit screen

bmp = Bitmap.new(640, 480)
bmp.fill_rect(0, 0, 640, 480, Color.new(0, 0, 0))

fnt = Font.new("Liberation Sans", 32)

bmp.font = fnt
bmp.draw_text(0, 0, 640, 240, "stretch_blt Test Suite", 1)
bmp.draw_text(0, 240, 640, 240, "Has Finished", 1)
spr.bitmap = bmp

Graphics.wait(1 * 60)

exit
