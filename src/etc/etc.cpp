/*
** etc.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "etc.h"

#include "serial-util.h"
#include "exception.h"

#include <SDL_types.h>
#include <SDL_pixels.h>

#include <cmath>

/* Unbounded defaults preserve Color's raw render values except for NaN. */
static double clampComponent(double value, double low = -INFINITY, double high = INFINITY)
{
	return std::isnan(value) ? 0.0 : clamp<double>(value, low, high);
}

Color::Color(double red, double green, double blue, double alpha)
{
	set(red, green, blue, alpha);
}

Color::Color(const Vec4 &norm)
	: norm(clampComponent(norm.x), clampComponent(norm.y),
	       clampComponent(norm.z), clampComponent(norm.w))
{
	updateExternal();
}

Color::Color(const Color &o)
    : red(o.red), green(o.green), blue(o.blue), alpha(o.alpha),
      norm(o.norm)
{}

bool Color::operator==(const Color &o) const
{
	return red   == o.red   &&
	       green == o.green &&
	       blue  == o.blue  &&
	       alpha == o.alpha;
}

const Color &Color::operator=(const Color &o)
{
	red   = o.red;
	green = o.green;
	blue  = o.blue;
	alpha = o.alpha;
	norm  = o.norm;

	return o;
}

void Color::set(double red, double green, double blue, double alpha)
{
	this->red   = clampComponent(red,   0, 255);
	this->green = clampComponent(green, 0, 255);
	this->blue  = clampComponent(blue,  0, 255);
	this->alpha = clampComponent(alpha, 0, 255);

	/* Preserve the existing constructor/set normalization of the input. */
	norm.x = clampComponent(red)   / 255;
	norm.y = clampComponent(green) / 255;
	norm.z = clampComponent(blue)  / 255;
	norm.w = clampComponent(alpha) / 255;
}

void Color::setRed(double value)
{
	red = clampComponent(value, 0, 255);
	norm.x = clampComponent(value, 0, 255) / 255;
}

void Color::setGreen(double value)
{
	green = clampComponent(value, 0, 255);
	norm.y = clampComponent(value, 0, 255) / 255;
}

void Color::setBlue(double value)
{
	blue = clampComponent(value, 0, 255);
	norm.z = clampComponent(value, 0, 255) / 255;
}

void Color::setAlpha(double value)
{
	alpha = clampComponent(value, 0, 255);
	norm.w = clampComponent(value, 0, 255) / 255;
}

/* Serializable */
int Color::serialSize() const
{
	return 4 * 8;
}

void Color::serialize(char *buffer) const
{
	writeDouble(&buffer, red);
	writeDouble(&buffer, green);
	writeDouble(&buffer, blue);
	writeDouble(&buffer, alpha);
}

Color *Color::deserialize(const char *data, int len)
{
	if (len != 32)
		throw Exception(Exception::ArgumentError, "Color: Serialized data invalid");

	Color *c = new Color();

	c->red   = clampComponent(readDouble(&data), 0, 255);
	c->green = clampComponent(readDouble(&data), 0, 255);
	c->blue  = clampComponent(readDouble(&data), 0, 255);
	c->alpha = clampComponent(readDouble(&data), 0, 255);
	c->updateInternal();

	return c;
}

void Color::updateInternal()
{
	norm.x = red   / 255;
	norm.y = green / 255;
	norm.z = blue  / 255;
	norm.w = alpha / 255;
}

void Color::updateExternal()
{
	red   = clampComponent(norm.x * 255, 0, 255);
	green = clampComponent(norm.y * 255, 0, 255);
	blue  = clampComponent(norm.z * 255, 0, 255);
	alpha = clampComponent(norm.w * 255, 0, 255);
}

SDL_Color Color::toSDLColor() const
{
	SDL_Color c;
	c.r = clampComponent(red, 0, 255);
	c.g = clampComponent(green, 0, 255);
	c.b = clampComponent(blue, 0, 255);
	c.a = clampComponent(alpha, 0, 255);

	return c;
}


Tone::Tone(double red, double green, double blue, double gray)
	: red(clampComponent(red, -255, 255)),
	  green(clampComponent(green, -255, 255)),
	  blue(clampComponent(blue, -255, 255)),
	  gray(clampComponent(gray, 0, 255))
{
	updateInternal();
}

Tone::Tone(const Tone &o)
    : red(o.red), green(o.green), blue(o.blue), gray(o.gray),
      norm(o.norm)
{}

bool Tone::operator==(const Tone &o) const
{
	return red   == o.red   &&
	       green == o.green &&
	       blue  == o.blue  &&
	       gray  == o.gray;
}

/* Tone's valueChanged is not a cheap notification: WindowVX connects it to a
 * whole CPU base recompose plus a whole-level texture upload. Stock VX Ace's
 * Window_Base#update runs `self.tone.set($game_system.window_tone)` on every
 * window on every frame, so the overwhelming majority of writes below store
 * the value that is already stored. Rect::set has guarded on equality since
 * upstream; every Tone mutator now does the same.
 *
 * Compare clamped components before notifying. Stores and normalization stay
 * unconditional so signed zero is preserved; NaN is compared as zero. */

void Tone::set(double red, double green, double blue, double gray)
{
	red   = clampComponent(red,   -255, 255);
	green = clampComponent(green, -255, 255);
	blue  = clampComponent(blue,  -255, 255);
	gray  = clampComponent(gray,     0, 255);

	const bool changed = (this->red   != red   ||
	                      this->green != green ||
	                      this->blue  != blue  ||
	                      this->gray  != gray);

	this->red   = red;
	this->green = green;
	this->blue  = blue;
	this->gray  = gray;

	updateInternal();

	if (changed)
		valueChanged();
}

const Tone& Tone::operator=(const Tone &o)
{
	/* This is where `tone.set(other_tone)` lands: binding/etc-binding.cpp's
	 * SET_FUN routes the one-argument form to *k = *other, and that is the
	 * form RGSS3 calls sixty times a second. */
	const bool changed = !(*this == o);

	red   = o.red;
	green = o.green;
	blue  = o.blue;
	gray  = o.gray;
	norm  = o.norm;

	if (changed)
		valueChanged();

	return o;
}

void Tone::setRed(double value)
{
	value = clampComponent(value, -255, 255);
	const bool changed = (red != value);

	red = value;
	norm.x = (float) clampComponent(value, -255, 255) / 255;

	if (changed)
		valueChanged();
}

void Tone::setGreen(double value)
{
	value = clampComponent(value, -255, 255);
	const bool changed = (green != value);

	green = value;
	norm.y = (float) clampComponent(value, -255, 255) / 255;

	if (changed)
		valueChanged();
}

void Tone::setBlue(double value)
{
	value = clampComponent(value, -255, 255);
	const bool changed = (blue != value);

	blue = value;
	norm.z = (float) clampComponent(value, -255, 255) / 255;

	if (changed)
		valueChanged();
}

void Tone::setGray(double value)
{
	value = clampComponent(value, 0, 255);
	const bool changed = (gray != value);

	gray = value;
	norm.w = (float) clampComponent(value, 0, 255) / 255;

	if (changed)
		valueChanged();
}

/* Serializable */
int Tone::serialSize() const
{
	return 4 * 8;
}

void Tone::serialize(char *buffer) const
{
	writeDouble(&buffer, red);
	writeDouble(&buffer, green);
	writeDouble(&buffer, blue);
	writeDouble(&buffer, gray);
}

Tone *Tone::deserialize(const char *data, int len)
{
	if (len != 32)
		throw Exception(Exception::ArgumentError, "Tone: Serialized data invalid");

	Tone *t = new Tone();

	t->red   = clampComponent(readDouble(&data), -255, 255);
	t->green = clampComponent(readDouble(&data), -255, 255);
	t->blue  = clampComponent(readDouble(&data), -255, 255);
	t->gray  = clampComponent(readDouble(&data), 0, 255);
	t->updateInternal();

	return t;
}

void Tone::updateInternal()
{
	norm.x = (float) clampComponent(red,   -255, 255) / 255;
	norm.y = (float) clampComponent(green, -255, 255) / 255;
	norm.z = (float) clampComponent(blue,  -255, 255) / 255;
	norm.w = (float) clampComponent(gray,     0, 255) / 255;
}


Rect::Rect(int x, int y, int width, int height)
    : x(x), y(y), width(width), height(height)
{}

Rect::Rect(const Rect &o)
    : x(o.x), y(o.y),
      width(o.width), height(o.height)
{}

Rect::Rect(const IntRect &r)
    : x(r.x), y(r.y), width(r.w), height(r.h)
{}

bool Rect::operator==(const Rect &o) const
{
	return x      == o.x     &&
	       y      == o.y     &&
	       width  == o.width &&
	       height == o.height;
}

void Rect::operator=(const IntRect &rect)
{
	x = rect.x;
	y = rect.y;
	width = rect.w;
	height = rect.h;
}

void Rect::set(int x, int y, int w, int h)
{
	if (this->x == x &&
	    this->y == y &&
	    width   == w &&
	    height  == h)
	{
		return;
	}

	this->x = x;
	this->y = y;
	width = w;
	height = h;
	valueChanged();
}

const Rect &Rect::operator=(const Rect &o)
{
	/* set(), empty() and the four component setters above all guard; this was
	 * the one Rect write that did not. A Rect valueChanged rebuilds a window's
	 * cursor vertices or re-derives a viewport's geometry and on-screen test,
	 * and a write of the value already stored has no business doing either.
	 * Expect little from it -- script code that assigns a rect usually is
	 * changing it -- it is here because the hole is the same hole.
	 *
	 * The other overload, operator=(const IntRect &), has never signalled and
	 * still does not: its one caller (Sprite::setBitmap) calls onSrcRectChange
	 * by hand on the next line. */
	const bool changed = !(*this == o);

	x      = o.x;
	y      = o.y;
	width  = o.width;
	height = o.height;

	if (changed)
		valueChanged();

	return o;
}

void Rect::empty()
{
	if (!(x || y || width || height))
		return;

	x = y = width = height = 0;
	valueChanged();
}

bool Rect::isEmpty() const
{
	return !(width && height);
}

void Rect::setX(int value)
{
	if (x == value)
		return;

	x = value;
	valueChanged();
}

void Rect::setY(int value)
{
	if (y == value)
		return;

	y = value;
	valueChanged();
}

void Rect::setWidth(int value)
{
	if (width == value)
		return;

	width = value;
	valueChanged();
}

void Rect::setHeight(int value)
{
	if (height == value)
		return;

	height = value;
	valueChanged();
}

int Rect::serialSize() const
{
	return 4 * 4;
}

void Rect::serialize(char *buffer) const
{
	writeInt32(&buffer, x);
	writeInt32(&buffer, y);
	writeInt32(&buffer, width);
	writeInt32(&buffer, height);
}

Rect *Rect::deserialize(const char *data, int len)
{
	if (len != 16)
		throw Exception(Exception::ArgumentError, "Rect: Serialized data invalid");

	Rect *r = new Rect();

	r->x      = readInt32(&data);
	r->y      = readInt32(&data);
	r->width  = readInt32(&data);
	r->height = readInt32(&data);

	return r;
}
