/*
** baseatlas.h
**
** Skyline allocator for the window-base atlas. Pure
** bookkeeping, no GL: GPUBudget owns the surface, windows own their slots.
*/

#ifndef BASEATLAS_H
#define BASEATLAS_H

#include <vector>

/* A base's slot in the atlas, gutter included: the base itself starts one
 * texel in. w == 0 means "no slot". */
struct BaseSlot
{
	int x, y, w, h;

	BaseSlot() : x(0), y(0), w(0), h(0) {}

	bool valid() const { return w > 0; }
	int innerX() const { return x + 1; }
	int innerY() const { return y + 1; }
	int innerW() const { return w - 2; }
	int innerH() const { return h - 2; }
};

class BaseAtlas
{
public:
	BaseAtlas() : width(0), height(0), live(0) {}

	void reset(int w, int h)
	{
		width = w;
		height = h;
		live = 0;
		sky.clear();
		if (w > 0)
			sky.push_back(Segment(0, 0, w));
	}

	bool ready() const { return width > 0; }
	int liveSlots() const { return live; }

	/* Lowest, then leftmost position on the skyline; false when nothing fits
	 * and the caller composes on the CPU instead. */
	bool alloc(int baseW, int baseH, BaseSlot &out)
	{
		const int w = baseW + 2, h = baseH + 2;

		if (!ready() || baseW <= 0 || baseH <= 0 || w > width || h > height)
			return false;

		int bestX = -1, bestY = 0;
		for (size_t i = 0; i < sky.size() && sky[i].x + w <= width; ++i)
		{
			int y = 0;
			for (size_t j = i; j < sky.size() && sky[j].x < sky[i].x + w; ++j)
				y = sky[j].y > y ? sky[j].y : y;

			if (y + h <= height && (bestX < 0 || y < bestY))
			{
				bestX = sky[i].x;
				bestY = y;
			}
		}

		if (bestX < 0)
			return false;

		setHeight(bestX, w, bestY + h);
		out.x = bestX;
		out.y = bestY;
		out.w = w;
		out.h = h;
		++live;

		return true;
	}

	/* A slot nothing was stacked on gives its space back at once; any other
	 * waits until the last live slot goes and the atlas starts empty again
	 * (every stock scene change disposes all of its windows). */
	void release(BaseSlot &slot)
	{
		if (!slot.valid())
			return;

		bool onTop = true;
		for (size_t i = 0; i < sky.size(); ++i)
			if (sky[i].x < slot.x + slot.w && sky[i].x + sky[i].w > slot.x &&
			    sky[i].y != slot.y + slot.h)
				onTop = false;

		if (onTop)
			setHeight(slot.x, slot.w, slot.y);

		if (live > 0 && --live == 0)
			reset(width, height);

		slot = BaseSlot();
	}

private:
	struct Segment
	{
		int x, y, w;
		Segment(int x, int y, int w) : x(x), y(y), w(w) {}
	};

	/* The skyline over [x, x + w) becomes y; segments stay sorted, cover
	 * [0, width) and never repeat a height side by side. */
	void setHeight(int x, int w, int y)
	{
		std::vector<Segment> next;

		for (size_t i = 0; i < sky.size(); ++i)
		{
			const Segment &s = sky[i];

			if (s.x < x)
				next.push_back(Segment(s.x, s.y, (s.x + s.w < x ? s.x + s.w : x) - s.x));
			if (s.x <= x && s.x + s.w > x)
				next.push_back(Segment(x, y, w));
			if (s.x + s.w > x + w)
				next.push_back(Segment(s.x > x + w ? s.x : x + w, s.y,
				                       s.x + s.w - (s.x > x + w ? s.x : x + w)));
		}

		sky.clear();
		for (size_t i = 0; i < next.size(); ++i)
		{
			if (!sky.empty() && sky.back().y == next[i].y)
				sky.back().w += next[i].w;
			else
				sky.push_back(next[i]);
		}
	}

	int width, height, live;
	std::vector<Segment> sky;
};

#endif // BASEATLAS_H
