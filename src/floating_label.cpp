/*
   Copyright (C) 2003 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project https://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "floating_label.hpp"

#include "display.hpp"
#include "font/text.hpp"
#include "log.hpp"
#include "video.hpp"

#include <map>
#include <set>
#include <stack>

static lg::log_domain log_font("font");
#define DBG_FT LOG_STREAM(debug, log_font)
#define LOG_FT LOG_STREAM(info, log_font)
#define WRN_FT LOG_STREAM(warn, log_font)
#define ERR_FT LOG_STREAM(err, log_font)

namespace
{
typedef std::map<int, font::floating_label> label_map;
label_map labels;
int label_id = 1;

std::stack<std::set<int>> label_contexts;
}

namespace font
{
floating_label::floating_label(const std::string& text, const surface& surf)
#if 0
	: img_(),
#else
	: surf_(surf)
	, buf_(nullptr)
	, buf_pos_()
#endif
	, fadeout_(true)
	, time_start_(0)
	, text_(text)
	, font_size_(SIZE_SMALL)
	, color_(NORMAL_COLOR)
	, bgcolor_()
	, bgalpha_(0)
	, xpos_(0)
	, ypos_(0)
	, xmove_(0)
	, ymove_(0)
	, lifetime_(-1)
	, width_(-1)
	, height_(-1)
	, clip_rect_(CVideo::get_singleton().screen_area())
	, visible_(true)
	, align_(CENTER_ALIGN)
	, border_(0)
	, scroll_(ANCHOR_LABEL_SCREEN)
	, use_markup_(true)
{
}

void floating_label::move(double xmove, double ymove)
{
	xpos_ += xmove;
	ypos_ += ymove;
}

int floating_label::xpos(std::size_t width) const
{
	int xpos = int(xpos_);
	if(align_ == font::CENTER_ALIGN) {
		xpos -= width / 2;
	} else if(align_ == font::RIGHT_ALIGN) {
		xpos -= width;
	}

	return xpos;
}

surface floating_label::create_surface()
{
	if(!surf_) {
		font::pango_text& text = font::get_text_renderer();

		text.set_link_aware(false)
			.set_font_size(font_size_)
			.set_font_style(font::pango_text::STYLE_NORMAL)
			.set_alignment(PANGO_ALIGN_LEFT)
			.set_foreground_color(color_)
			.set_maximum_width(width_ < 0 ? clip_rect_.w : width_)
			.set_maximum_height(height_ < 0 ? clip_rect_.h : height_, true)
			.set_ellipse_mode(PANGO_ELLIPSIZE_END)
			.set_characters_per_line(0);

		// ignore last '\n'
		if(!text_.empty() && *(text_.rbegin()) == '\n') {
			text.set_text(std::string(text_.begin(), text_.end() - 1), use_markup_);
		} else {
			text.set_text(text_, use_markup_);
		}

		surface foreground = text.render();

		if(foreground == nullptr) {
			ERR_FT << "could not create floating label's text" << std::endl;
			return nullptr;
		}

		// combine foreground text with its background
		if(bgalpha_ != 0) {
			// background is a dark tooltip box
			surface background(foreground->w + border_ * 2, foreground->h + border_ * 2);

			if(background == nullptr) {
				ERR_FT << "could not create tooltip box" << std::endl;
				return surf_ = foreground;
			}

			uint32_t color = SDL_MapRGBA(foreground->format, bgcolor_.r, bgcolor_.g, bgcolor_.b, bgalpha_);
			sdl::fill_surface_rect(background, nullptr, color);

			// we make the text less transparent, because the blitting on the
			// dark background will darken the anti-aliased part.
			// This 1.13 value seems to restore the brightness of version 1.4
			// (where the text was blitted directly on screen)
			adjust_surface_alpha(foreground, ftofxp(1.13));

			SDL_Rect r{border_, border_, 0, 0};
			adjust_surface_alpha(foreground, SDL_ALPHA_OPAQUE);
			sdl_blit(foreground, nullptr, background, &r);

			surf_ = background;
		} else {
			// background is blurred shadow of the text
			surface background(foreground->w + 4, foreground->h + 4);
			sdl::fill_surface_rect(background, nullptr, 0);
			SDL_Rect r{2, 2, 0, 0};
			sdl_blit(foreground, nullptr, background, &r);
			background = shadow_image(background);

			if(background == nullptr) {
				ERR_FT << "could not create floating label's shadow" << std::endl;
				return surf_ = foreground;
			}
			sdl_blit(foreground, nullptr, background, &r);
			surf_ = background;
		}
	}

	return surf_;
}

void floating_label::draw(int time, surface screen)
{
	if(!visible_) {
		buf_ = nullptr;
		return;
	}

	if(screen == nullptr) {
		return;
	}

	create_surface();
	if(surf_ == nullptr) {
		return;
	}

	if(buf_ == nullptr) {
		buf_ = surface(surf_->w, surf_->h);
		if(buf_ == nullptr) {
			return;
		}
	}

	SDL_Point pos = get_loc(time);
	buf_pos_ = sdl::create_rect(pos.x, pos.y, surf_->w, surf_->h);
	const clip_rect_setter clip_setter(screen, &clip_rect_);
	//important: make a copy of buf_pos_ because sdl_blit modifies dst_rect.
	SDL_Rect rect = buf_pos_;
	sdl_copy_portion(screen, &rect, buf_, nullptr);
	sdl_blit(get_surface(time), nullptr, screen, &rect);
}

void floating_label::set_lifetime(int lifetime)
{
	lifetime_ = lifetime;
	time_start_	= SDL_GetTicks();
}


SDL_Point floating_label::get_loc(int time)
{
	int time_alive = get_time_alive(time);
	return {
		static_cast<int>(time_alive * xmove_ + xpos(surf_->w)),
		static_cast<int>(time_alive * ymove_ + ypos_)
 	};
}

surface floating_label::get_surface(int time)
{
	if(fadeout_ && lifetime_ >= 0 && surf_ != nullptr) {
		// fade out moving floating labels
		int time_alive = get_time_alive(time);
		int alpha_add = -255 * time_alive / lifetime_;
		return adjust_surface_alpha_add(surf_, alpha_add);
	}
	return surf_;
}

void floating_label::undraw(surface screen)
{
	if(screen == nullptr || buf_ == nullptr) {
		return;
	}

	const clip_rect_setter clip_setter(screen, &clip_rect_);
	SDL_Rect rect = buf_pos_;
	sdl_blit(buf_, nullptr, screen, &rect);
}

int add_floating_label(const floating_label& flabel)
{
	if(label_contexts.empty()) {
		return 0;
	}

	++label_id;
	labels.emplace(label_id, flabel);
	label_contexts.top().insert(label_id);
	return label_id;
}

void move_floating_label(int handle, double xmove, double ymove)
{
	const label_map::iterator i = labels.find(handle);
	if(i != labels.end()) {
		i->second.move(xmove, ymove);
	}
}

void scroll_floating_labels(double xmove, double ymove)
{
	for(label_map::iterator i = labels.begin(); i != labels.end(); ++i) {
		if(i->second.scroll() == ANCHOR_LABEL_MAP) {
			i->second.move(xmove, ymove);
		}
	}
}

void remove_floating_label(int handle)
{
	const label_map::iterator i = labels.find(handle);
	if(i != labels.end()) {
		labels.erase(i);
	}

	if(!label_contexts.empty()) {
		label_contexts.top().erase(handle);
	}
}

void show_floating_label(int handle, bool value)
{
	const label_map::iterator i = labels.find(handle);
	if(i != labels.end()) {
		i->second.show(value);
	}
}

SDL_Rect get_floating_label_rect(int handle)
{
	const label_map::iterator i = labels.find(handle);
	if(i != labels.end()) {
		const surface surf = i->second.create_surface();
		if(surf != nullptr) {
			return {0, 0, surf->w, surf->h};
		}
	}
	return sdl::empty_rect;
}

floating_label_context::floating_label_context()
{
	//TODO: 'pause' floating labels in other contexrs
	label_contexts.emplace();
}

floating_label_context::~floating_label_context()
{
	//TODO: 'pause' floating labels in other contexrs
	const std::set<int>& context = label_contexts.top();

	while(!context.empty()) {
		// Remove_floating_label removes the passed label from the context.
		// This loop removes a different label in every iteration.
		remove_floating_label(*context.begin());
	}

	label_contexts.pop();
}

void draw_floating_labels(surface screen)
{
	if(label_contexts.empty()) {
		return;
	}
	int time = SDL_GetTicks();

	const std::set<int>& context = label_contexts.top();

	// draw the labels in the order they were added, so later added labels (likely to be tooltips)
	// are displayed over earlier added labels.
	for(label_map::iterator i = labels.begin(); i != labels.end(); ++i) {
		if(context.count(i->first) > 0) {
			i->second.draw(time, screen);
		}
	}
}

void undraw_floating_labels(surface screen)
{
	if(label_contexts.empty()) {
		return;
	}
	int time = SDL_GetTicks();

	std::set<int>& context = label_contexts.top();

	//undraw labels in reverse order, so that a LIFO process occurs, and the screen is restored
	//into the exact state it started in.
	for(label_map::reverse_iterator i = labels.rbegin(); i != labels.rend(); ++i) {
		if(context.count(i->first) > 0) {
			i->second.undraw(screen);
		}
	}

	//remove expired labels
	for(label_map::iterator j = labels.begin(); j != labels.end(); ) {
		if(context.count(j->first) > 0 && j->second.expired(time)) {
			context.erase(j->first);
			labels.erase(j++);
		} else {
			++j;
		}
	}
}
}
