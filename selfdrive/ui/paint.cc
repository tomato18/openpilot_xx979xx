#include "ui.hpp"
#include <assert.h>
#include <map>
#include <cmath>
#include <iostream>
#include "common/util.h"

#define NANOVG_GLES3_IMPLEMENTATION
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

extern "C"{
#include "common/glutil.h"
}

#include "paint.hpp"
#include "sidebar.hpp"

// TODO: this is also hardcoded in common/transformations/camera.py
const mat3 intrinsic_matrix = (mat3){{
  910., 0., 582.,
  0., 910., 437.,
  0.,   0.,   1.
}};

const uint8_t alert_colors[][4] = {
  [STATUS_OFFROAD] = {0x07, 0x23, 0x39, 0xf1},
  [STATUS_DISENGAGED] = {0x17, 0x33, 0x49, 0xc8},
  [STATUS_ENGAGED] = {0x17, 0x86, 0x44, 0x01},
  [STATUS_ENGAGED_OPLONG] = {0x69, 0x69, 0x69, 0x01},
  [STATUS_WARNING] = {0xDA, 0x6F, 0x25, 0x01},
  [STATUS_ALERT] = {0xC9, 0x22, 0x31, 0xf1},
};

// Projects a point in car to space to the corresponding point in full frame
// image space.
vec3 car_space_to_full_frame(const UIState *s, vec4 car_space_projective) {
  const UIScene *scene = &s->scene;

  // We'll call the car space point p.
  // First project into normalized image coordinates with the extrinsics matrix.
  const vec4 Ep4 = matvecmul(scene->extrinsic_matrix, car_space_projective);

  // The last entry is zero because of how we store E (to use matvecmul).
  const vec3 Ep = {{Ep4.v[0], Ep4.v[1], Ep4.v[2]}};
  const vec3 KEp = matvecmul3(intrinsic_matrix, Ep);

  // Project.
  const vec3 p_image = {{KEp.v[0] / KEp.v[2], KEp.v[1] / KEp.v[2], 1.}};
  return p_image;
}

// Calculate an interpolation between two numbers at a specific increment
static float lerp(float v0, float v1, float t) {
  return (1 - t) * v0 + t * v1;
}

static void ui_draw_text(NVGcontext *vg, float x, float y, const char* string, float size, NVGcolor color, int font){
  nvgFontFaceId(vg, font);
  nvgFontSize(vg, size);
  nvgFillColor(vg, color);
  nvgText(vg, x, y, string, NULL);
}

static void draw_chevron(UIState *s, float x_in, float y_in, float sz,
                          NVGcolor fillColor, NVGcolor glowColor) {
  const vec4 p_car_space = (vec4){{x_in, y_in, 0., 1.}};
  const vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);

  float x = p_full_frame.v[0];
  float y = p_full_frame.v[1];
  if (x < 0 || y < 0.){
    return;
  }

  sz *= 30;
  sz /= (x_in / 3 + 30);
  if (sz > 30) sz = 30;
  if (sz < 15) sz = 15;

  // glow
  float g_xo = sz/5;
  float g_yo = sz/10;
  nvgBeginPath(s->vg);
  nvgMoveTo(s->vg, x+(sz*1.35)+g_xo, y+sz+g_yo);
  nvgLineTo(s->vg, x, y-g_xo);
  nvgLineTo(s->vg, x-(sz*1.35)-g_xo, y+sz+g_yo);
  nvgClosePath(s->vg);
  nvgFillColor(s->vg, glowColor);
  nvgFill(s->vg);

  // chevron
  nvgBeginPath(s->vg);
  nvgMoveTo(s->vg, x+(sz*1.25), y+sz);
  nvgLineTo(s->vg, x, y);
  nvgLineTo(s->vg, x-(sz*1.25), y+sz);
  nvgClosePath(s->vg);
  nvgFillColor(s->vg, fillColor);
  nvgFill(s->vg);
}

static void ui_draw_circle_image(NVGcontext *vg, float x, float y, int size, int image, NVGcolor color, float img_alpha, int img_y = 0) {
  const int img_size = size * 1.5;
  nvgBeginPath(vg);
  nvgCircle(vg, x, y + (bdr_s * 1.5), size);
  nvgFillColor(vg, color);
  nvgFill(vg);
  ui_draw_image(vg, x - (img_size / 2), img_y ? img_y : y - (size / 4), img_size, img_size, image, img_alpha);
}

static void ui_draw_circle_image(NVGcontext *vg, float x, float y, int size, int image, bool active) {
  float bg_alpha = active ? 0.3f : 0.1f;
  float img_alpha = active ? 1.0f : 0.15f;
  ui_draw_circle_image(vg, x, y, size, image, nvgRGBA(0, 0, 0, (255 * bg_alpha)), img_alpha);
}

static void draw_lead(UIState *s, const cereal::RadarState::LeadData::Reader &lead){
  // Draw lead car indicator
  float fillAlpha = 0;
  float speedBuff = 10.;
  float leadBuff = 40.;
  float d_rel = lead.getDRel();
  float v_rel = lead.getVRel();
  if (d_rel < leadBuff) {
    fillAlpha = 255*(1.0-(d_rel/leadBuff));
    if (v_rel < 0) {
      fillAlpha += 255*(-1*(v_rel/speedBuff));
    }
    fillAlpha = (int)(fmin(fillAlpha, 255));
  }
  if (s->longitudinal_control) {
    draw_chevron(s, d_rel + 3, lead.getYRel(), 25, nvgRGBA(201, 34, 49, fillAlpha), COLOR_YELLOW);
  }
  else {
    draw_chevron(s, d_rel + 3, lead.getYRel(), 25, nvgRGBA(165, 255, 135, fillAlpha), COLOR_GREEN);
  }
}

static void ui_draw_lane_line(UIState *s, const model_path_vertices_data *pvd, NVGcolor color) {
  if (pvd->cnt == 0) return;

  nvgBeginPath(s->vg);
  nvgMoveTo(s->vg, pvd->v[0].x, pvd->v[0].y);
  for (int i=1; i<pvd->cnt; i++) {
    nvgLineTo(s->vg, pvd->v[i].x, pvd->v[i].y);
  }
  nvgClosePath(s->vg);
  nvgFillColor(s->vg, color);
  nvgFill(s->vg);
}

static void update_track_data(UIState *s, bool is_mpc, track_vertices_data *pvd) {
  const UIScene *scene = &s->scene;
  const float *points = scene->path_points;
  const float *mpc_x_coords = &scene->mpc_x[0];
  const float *mpc_y_coords = &scene->mpc_y[0];

  float off = is_mpc?0.3:0.5;
  float lead_d = scene->lead_data[0].getDRel()*2.;
  float path_height = is_mpc?(lead_d>5.)?fmin(lead_d, 25.)-fmin(lead_d*0.35, 10.):20.
                            :(lead_d>0.)?fmin(lead_d, 50.)-fmin(lead_d*0.35, 10.):49.;
  path_height = fmin(path_height, scene->model.getPath().getValidLen());
  pvd->cnt = 0;
  // left side up
  for (int i=0; i<=path_height; i++) {
    float px, py, mpx;
    if (is_mpc) {
      mpx = i==0?0.0:mpc_x_coords[i];
      px = lerp(mpx+1.0, mpx, i/100.0);
      py = mpc_y_coords[i] - off;
    } else {
      px = lerp(i+1.0, i, i/100.0);
      py = points[i] - off;
    }

    vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);
    if (p_full_frame.v[0] < 0. || p_full_frame.v[1] < 0.) {
      continue;
    }
    pvd->v[pvd->cnt].x = p_full_frame.v[0];
    pvd->v[pvd->cnt].y = p_full_frame.v[1];
    pvd->cnt += 1;
  }

  // right side down
  for (int i=path_height; i>=0; i--) {
    float px, py, mpx;
    if (is_mpc) {
      mpx = i==0?0.0:mpc_x_coords[i];
      px = lerp(mpx+1.0, mpx, i/100.0);
      py = mpc_y_coords[i] + off;
    } else {
      px = lerp(i+1.0, i, i/100.0);
      py = points[i] + off;
    }

    vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);
    if (p_full_frame.v[0] < 0. || p_full_frame.v[1] < 0.) {
      continue;
    }
    pvd->v[pvd->cnt].x = p_full_frame.v[0];
    pvd->v[pvd->cnt].y = p_full_frame.v[1];
    pvd->cnt += 1;
  }
}

static void update_all_track_data(UIState *s) {
  const UIScene *scene = &s->scene;
  // Draw vision path
  update_track_data(s, false, &s->track_vertices[0]);

  if (scene->controls_state.getEnabled()) {
    // Draw MPC path when engaged
    update_track_data(s, true, &s->track_vertices[1]);
  }
}

static void ui_draw_track(UIState *s, bool is_mpc, track_vertices_data *pvd) {
 if (pvd->cnt == 0) return;

  nvgBeginPath(s->vg);
  nvgMoveTo(s->vg, pvd->v[0].x, pvd->v[0].y);
  for (int i=1; i<pvd->cnt; i++) {
    nvgLineTo(s->vg, pvd->v[i].x, pvd->v[i].y);
  }
  nvgClosePath(s->vg);

  NVGpaint track_bg;
  if (is_mpc) {
    // Draw colored MPC track
    const Color clr = bg_colors[s->status];
    track_bg = nvgLinearGradient(s->vg, s->fb_w, s->fb_h, s->fb_w, s->fb_h*.4,
      nvgRGBA(clr.r, clr.g, clr.b, 255), nvgRGBA(clr.r, clr.g, clr.b, 255/2));
  } else {
    // Draw white vision track
    track_bg = nvgLinearGradient(s->vg, s->fb_w, s->fb_h, s->fb_w, s->fb_h*.4,
      COLOR_WHITE, COLOR_WHITE_ALPHA(0));
  }
  nvgFillPaint(s->vg, track_bg);
  nvgFill(s->vg);
}

static void draw_frame(UIState *s) {
  mat4 *out_mat;
  if (s->scene.frontview) {
    glBindVertexArray(s->frame_vao[1]);
    out_mat = &s->front_frame_mat;
  } else {
    glBindVertexArray(s->frame_vao[0]);
    out_mat = &s->rear_frame_mat;
  }
  glActiveTexture(GL_TEXTURE0);

  if (s->stream.last_idx >= 0) {
    glBindTexture(GL_TEXTURE_2D, s->frame_texs[s->stream.last_idx]);
#ifndef QCOM
    // this is handled in ion on QCOM
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, s->stream.bufs_info.width, s->stream.bufs_info.height,
                 0, GL_RGB, GL_UNSIGNED_BYTE, s->priv_hnds[s->stream.last_idx]);
#endif
  }

  glUseProgram(s->frame_program);
  glUniform1i(s->frame_texture_loc, 0);
  glUniformMatrix4fv(s->frame_transform_loc, 1, GL_TRUE, out_mat->v);

  assert(glGetError() == GL_NO_ERROR);
  glEnableVertexAttribArray(0);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, (const void*)0);
  glDisableVertexAttribArray(0);
  glBindVertexArray(0);
}

static inline bool valid_frame_pt(UIState *s, float x, float y) {
  return x >= 0 && x <= s->stream.bufs_info.width && y >= 0 && y <= s->stream.bufs_info.height;
}

static void update_lane_line_data(UIState *s, const float *points, float off, model_path_vertices_data *pvd, float valid_len) {
  pvd->cnt = 0;
  int rcount = fmin(MODEL_PATH_MAX_VERTICES_CNT / 2, valid_len);
  for (int i = 0; i < rcount; i++) {
    float px = (float)i;
    float py = points[i] - off;
    const vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    const vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);
    if(!valid_frame_pt(s, p_full_frame.v[0], p_full_frame.v[1]))
      continue;
    pvd->v[pvd->cnt].x = p_full_frame.v[0];
    pvd->v[pvd->cnt].y = p_full_frame.v[1];
    pvd->cnt += 1;
  }
  for (int i = rcount - 1; i > 0; i--) {
    float px = (float)i;
    float py = points[i] + off;
    const vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    const vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);
    if(!valid_frame_pt(s, p_full_frame.v[0], p_full_frame.v[1]))
      continue;
    pvd->v[pvd->cnt].x = p_full_frame.v[0];
    pvd->v[pvd->cnt].y = p_full_frame.v[1];
    pvd->cnt += 1;
  }
}

static void update_all_lane_lines_data(UIState *s, const cereal::ModelData::PathData::Reader &path, const float *points, model_path_vertices_data *pstart) {
  update_lane_line_data(s, points, 0.025*path.getProb(), pstart, path.getValidLen());
  update_lane_line_data(s, points, fmin(path.getStd(), 0.7), pstart + 1, path.getValidLen());
}

static void ui_draw_lane(UIState *s,  model_path_vertices_data *pstart, NVGcolor color) {
  ui_draw_lane_line(s, pstart, color);
  color.a /= 25;
  ui_draw_lane_line(s, pstart + 1, color);
}

static void ui_draw_vision_lanes(UIState *s) {
  const UIScene *scene = &s->scene;
  model_path_vertices_data *pvd = &s->model_path_vertices[0];
  if(s->sm->updated("model")) {
    update_all_lane_lines_data(s, scene->model.getLeftLane(), scene->left_lane_points, pvd);
    update_all_lane_lines_data(s, scene->model.getRightLane(), scene->right_lane_points, pvd + MODEL_LANE_PATH_CNT);
  }

  // Draw left lane edge
  ui_draw_lane(s, pvd, nvgRGBAf(1.0, 1.0, 1.0, scene->model.getLeftLane().getProb()));

  // Draw right lane edge
  ui_draw_lane(s, pvd + MODEL_LANE_PATH_CNT, nvgRGBAf(1.0, 1.0, 1.0, scene->model.getRightLane().getProb()));

  if(s->sm->updated("radarState")) {
    update_all_track_data(s);
  }

  // Draw vision path
  ui_draw_track(s, false, &s->track_vertices[0]);
  if (scene->controls_state.getEnabled()) {
    // Draw MPC path when engaged
    ui_draw_track(s, true, &s->track_vertices[1]);
  }
}

// Draw all world space objects.
static void ui_draw_world(UIState *s) {
  const UIScene *scene = &s->scene;
  const Rect &viz_rect = scene->viz_rect;
  const int viz_w = s->fb_w - bdr_s * 2;
  const int inner_height = float(viz_w) * s->fb_h / s->fb_w;
  nvgSave(s->vg);
  nvgScissor(s->vg, viz_rect.x, viz_rect.y, viz_rect.w, viz_rect.h);

  nvgTranslate(s->vg, viz_rect.x+scene->ui_viz_ro, viz_rect.y + (viz_rect.h-inner_height)/2.0);
  nvgScale(s->vg, (float)viz_w / s->fb_w, (float)inner_height / s->fb_h);

  float w = 1440.0f; // Why 1440?
  nvgTranslate(s->vg, (s->fb_w - w) / 2.0f, 0.0);

  nvgTranslate(s->vg, -w / 2, -1080.0f / 2);
  nvgScale(s->vg, 2.0, 2.0);
  nvgScale(s->vg, w / s->stream.bufs_info.width, 1080.0f / s->stream.bufs_info.height);

  // Draw lane edges and vision/mpc tracks
  ui_draw_vision_lanes(s);

  // Draw lead indicators if openpilot is handling longitudinal
    if (scene->lead_data[0].getStatus()) {
      draw_lead(s, scene->lead_data[0]);
    }
    if (scene->lead_data[1].getStatus() && (std::abs(scene->lead_data[0].getDRel() - scene->lead_data[1].getDRel()) > 1.0)) {
      draw_lead(s, scene->lead_data[1]);
  }
  nvgRestore(s->vg);
}

static void ui_draw_vision_maxspeed(UIState *s) {
  char maxspeed_str[32];
  float maxspeed = s->scene.controls_state.getVCruise();
  int maxspeed_calc = maxspeed * 0.6225 + 0.5;
  if (s->is_metric) {
    maxspeed_calc = maxspeed + 0.5;
  }

  bool is_cruise_set = (maxspeed != 0 && maxspeed != SET_SPEED_NA);

  int viz_maxspeed_w = 184;
  int viz_maxspeed_h = 202;
  int viz_maxspeed_x = s->scene.viz_rect.x + (bdr_s*2);
  int viz_maxspeed_y = s->scene.viz_rect.y + (bdr_s*1.5);
  int viz_maxspeed_xo = 180;

  viz_maxspeed_xo = 0;

  // Draw Background
  ui_draw_rect(s->vg, viz_maxspeed_x, viz_maxspeed_y, viz_maxspeed_w, viz_maxspeed_h, COLOR_BLACK_ALPHA(100), 30);

  // Draw Border
  NVGcolor color = COLOR_WHITE_ALPHA(100);
  ui_draw_rect(s->vg, viz_maxspeed_x, viz_maxspeed_y, viz_maxspeed_w, viz_maxspeed_h, color, 20, 10);

  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
  const int text_x = viz_maxspeed_x + (viz_maxspeed_xo / 2) + (viz_maxspeed_w / 2);
  ui_draw_text(s->vg, text_x, 148, "MAX", 26 * 2.5, COLOR_WHITE_ALPHA(is_cruise_set ? 200 : 100), s->font_sans_regular);

  if (is_cruise_set) {
    snprintf(maxspeed_str, sizeof(maxspeed_str), "%d", maxspeed_calc);
    ui_draw_text(s->vg, text_x, 242, maxspeed_str, 48 * 2.5, COLOR_WHITE, s->font_sans_bold);
  } else {
    ui_draw_text(s->vg, text_x, 242, "N/A", 42 * 2.5, COLOR_WHITE_ALPHA(100), s->font_sans_semibold);
  }
}

static void ui_draw_vision_speed(UIState *s) {
  const Rect &viz_rect = s->scene.viz_rect;
  const UIScene *scene = &s->scene;
  float v_ego = s->scene.controls_state.getVEgo();
  float speed = v_ego * 2.2369363 + 0.5;
  if (s->is_metric){
    speed = v_ego * 3.6 + 0.5;
  }
  const int viz_speed_w = 280;
  const int viz_speed_x = viz_rect.centerX() - viz_speed_w/2;
  char speed_str[32];

  // turning blinker from kegman
  if(scene->leftBlinker) {
    nvgBeginPath(s->vg);
    nvgMoveTo(s->vg, viz_speed_x, viz_rect.y + header_h/4);
    nvgLineTo(s->vg, viz_speed_x - viz_speed_w/2, viz_rect.y + header_h/4 + header_h/4);
    nvgLineTo(s->vg, viz_speed_x, viz_rect.y + header_h/2 + header_h/4);
    nvgClosePath(s->vg);
    nvgFillColor(s->vg, nvgRGBA(23,134,68,scene->blinker_blinkingrate>=50?210:60));
    nvgFill(s->vg);
  }
  if(scene->rightBlinker) {
    nvgBeginPath(s->vg);
    nvgMoveTo(s->vg, viz_speed_x+viz_speed_w, viz_rect.y + header_h/4);
    nvgLineTo(s->vg, viz_speed_x+viz_speed_w + viz_speed_w/2, viz_rect.y + header_h/4 + header_h/4);
    nvgLineTo(s->vg, viz_speed_x+viz_speed_w, viz_rect.y + header_h/2 + header_h/4);
    nvgClosePath(s->vg);
    nvgFillColor(s->vg, nvgRGBA(23,134,68,scene->blinker_blinkingrate>=50?210:60));
    nvgFill(s->vg);
    }
  if(scene->leftBlinker || scene->rightBlinker) {
    s->scene.blinker_blinkingrate -= 3;
    if(scene->blinker_blinkingrate<0) s->scene.blinker_blinkingrate = 120;
  }

  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

  snprintf(speed_str, sizeof(speed_str), "%d", (int)speed);
  ui_draw_text(s->vg, viz_rect.centerX(), 240, speed_str, 96*2.5, COLOR_WHITE, s->font_sans_bold);
  ui_draw_text(s->vg, viz_rect.centerX(), 320, s->is_metric?"km/h":"mph", 36*2.5, COLOR_WHITE_ALPHA(200), s->font_sans_regular);
}

static void ui_draw_vision_event(UIState *s) {
  const int viz_event_w = 220;
  const int viz_event_x = s->scene.viz_rect.right() - (viz_event_w + bdr_s*2);
  const int viz_event_y = s->scene.viz_rect.y + (bdr_s*1.5);
  if (s->scene.controls_state.getDecelForModel() && s->scene.controls_state.getEnabled()) {
    // draw winding road sign
    const int img_turn_size = 160*1.5;
    ui_draw_image(s->vg, viz_event_x - (img_turn_size / 4), viz_event_y + bdr_s - 25, img_turn_size, img_turn_size, s->img_turn, 1.0f);
  } else {
    // draw steering wheel
    const int bg_wheel_size = 96;
    const int bg_wheel_x = viz_event_x + (viz_event_w-bg_wheel_size);
    const int bg_wheel_y = viz_event_y + (bg_wheel_size/2);
    const int img_wheel_size = bg_wheel_size*1.5;
    const int img_wheel_x = bg_wheel_x-(img_wheel_size/2);
    const int img_wheel_y = bg_wheel_y-25;
    const float img_rotation = s->scene.angleSteers/180*3.141592;
    float img_wheel_alpha = 0.1f;
    bool is_engaged = (s->status == STATUS_ENGAGED) && !s->scene.steerOverride;
    bool is_warning = (s->status == STATUS_WARNING);
    bool is_engageable = s->scene.controls_state.getEngageable();
    bool is_oplong = (s->status == STATUS_ENGAGED_OPLONG);

    if (is_engaged || is_warning || is_engageable) {
      nvgBeginPath(s->vg);
      nvgCircle(s->vg, bg_wheel_x, (bg_wheel_y + (bdr_s*1.5)), bg_wheel_size);
      if (is_oplong) {
        nvgFillColor(s->vg, nvgRGBA(105, 105, 105, 105));
      }
      else if (is_engaged) {
        nvgFillColor(s->vg, nvgRGBA(23, 134, 68, 255));
      } else if (is_warning) {
        nvgFillColor(s->vg, COLOR_OCHRE);
      } else if (is_engageable) {
        nvgFillColor(s->vg, nvgRGBA(23, 51, 73, 255));
      }
      nvgFill(s->vg);
      img_wheel_alpha = 1.0f;
    }
    nvgSave(s->vg);
    nvgTranslate(s->vg,bg_wheel_x,(bg_wheel_y + (bdr_s*1.5)));
    nvgRotate(s->vg,-img_rotation);
    nvgBeginPath(s->vg);
    NVGpaint imgPaint = nvgImagePattern(s->vg, img_wheel_x-bg_wheel_x, img_wheel_y-(bg_wheel_y + (bdr_s*1.5)),
	img_wheel_size, img_wheel_size, 0, s->img_wheel, img_wheel_alpha);
    nvgRect(s->vg, img_wheel_x-bg_wheel_x, img_wheel_y-(bg_wheel_y + (bdr_s*1.5)), img_wheel_size, img_wheel_size);
    nvgFillPaint(s->vg, imgPaint);
    nvgFill(s->vg);
    nvgRestore(s->vg);
    }
}

static void ui_draw_vision_face(UIState *s) {
  const int face_size = 96;
  const int face_x = (s->scene.viz_rect.x + face_size + (bdr_s * 2));
  const int face_y = (s->scene.viz_rect.bottom() - footer_h + ((footer_h - face_size) / 2));
  ui_draw_circle_image(s->vg, face_x, face_y, face_size, s->img_face, s->scene.dmonitoring_state.getFaceDetected());
}

static void ui_draw_driver_view(UIState *s) {
  const UIScene *scene = &s->scene;
  s->scene.uilayout_sidebarcollapsed = true;
  const Rect &viz_rect = s->scene.viz_rect;
  const int ff_xoffset = 32;
  const int frame_x = viz_rect.x;
  const int frame_w = viz_rect.w;
  const int valid_frame_w = 4 * viz_rect.h / 3;
  const int box_y = viz_rect.y;
  const int box_h  = viz_rect.h;
  const int valid_frame_x = frame_x + (frame_w - valid_frame_w) / 2 + ff_xoffset;

  // blackout
  NVGpaint gradient = nvgLinearGradient(s->vg, scene->is_rhd ? valid_frame_x : (valid_frame_x + valid_frame_w),
                                        box_y,
                                        scene->is_rhd ? (valid_frame_w - box_h / 2) : (valid_frame_x + box_h / 2), box_y,
                                        COLOR_BLACK, COLOR_BLACK_ALPHA(0));
  ui_draw_rect(s->vg, scene->is_rhd ? valid_frame_x : (valid_frame_x + box_h / 2), box_y, valid_frame_w - box_h / 2, box_h, gradient);
  ui_draw_rect(s->vg, scene->is_rhd ? valid_frame_x : valid_frame_x + box_h / 2, box_y, valid_frame_w - box_h / 2, box_h, COLOR_BLACK_ALPHA(144));

  // borders
  ui_draw_rect(s->vg, frame_x, box_y, valid_frame_x - frame_x, box_h, nvgRGBA(23, 51, 73, 255));
  ui_draw_rect(s->vg, valid_frame_x + valid_frame_w, box_y, frame_w - valid_frame_w - (valid_frame_x - frame_x), box_h, nvgRGBA(23, 51, 73, 255));

  // draw face box
  if (scene->dmonitoring_state.getFaceDetected()) {
    auto fxy_list = scene->driver_state.getFacePosition();
    const float face_x = fxy_list[0];
    const float face_y = fxy_list[1];
    float fbox_x;
    float fbox_y = box_y + (face_y + 0.5) * box_h - 0.5 * 0.6 * box_h / 2;;
    if (!scene->is_rhd) {
      fbox_x = valid_frame_x + (1 - (face_x + 0.5)) * (box_h / 2) - 0.5 * 0.6 * box_h / 2;
    } else {
      fbox_x = valid_frame_x + valid_frame_w - box_h / 2 + (face_x + 0.5) * (box_h / 2) - 0.5 * 0.6 * box_h / 2;
    }

    if (std::abs(face_x) <= 0.35 && std::abs(face_y) <= 0.4) {
      ui_draw_rect(s->vg, fbox_x, fbox_y, 0.6 * box_h / 2, 0.6 * box_h / 2,
                   nvgRGBAf(1.0, 1.0, 1.0, 0.8 - ((std::abs(face_x) > std::abs(face_y) ? std::abs(face_x) : std::abs(face_y))) * 0.6 / 0.375),
                   35, 10);
    } else {
      ui_draw_rect(s->vg, fbox_x, fbox_y, 0.6 * box_h / 2, 0.6 * box_h / 2, nvgRGBAf(1.0, 1.0, 1.0, 0.2), 35, 10);
    }
  }

  // draw face icon
  const int face_size = 85;
  const int x = (valid_frame_x + face_size + (bdr_s * 2)) + (scene->is_rhd ? valid_frame_w - box_h / 2:0);
  const int y = (box_y + box_h - face_size - bdr_s - (bdr_s * 1.5));
  ui_draw_circle_image(s->vg, x, y, face_size, s->img_face, scene->dmonitoring_state.getFaceDetected());
}

static void ui_draw_vision_brake(UIState *s) {
  const UIScene *scene = &s->scene;
  const Rect &viz_rect = s->scene.viz_rect;
  const int brake_size = 96;
  const int brake_x = (viz_rect.x + (brake_size * 3) + (bdr_s * 4));
  const int brake_y = (footer_y + ((footer_h - brake_size) / 2));
  const int brake_img_size = (brake_size * 1.5);
  const int brake_img_x = (brake_x - (brake_img_size / 2));
  const int brake_img_y = (brake_y - (brake_size / 4));

  bool brake_valid = scene->brakeLights;
  float brake_img_alpha = brake_valid ? 1.0f : 0.15f;
  float brake_bg_alpha = brake_valid ? 0.3f : 0.1f;
  NVGcolor brake_bg = nvgRGBA(0, 0, 0, (255 * brake_bg_alpha));
  NVGpaint brake_img = nvgImagePattern(s->vg, brake_img_x, brake_img_y,
                                       brake_img_size, brake_img_size, 0,
                                       s->img_brake, brake_img_alpha);

  nvgBeginPath(s->vg);
  nvgCircle(s->vg, brake_x, (brake_y + (bdr_s * 1.5)), brake_size);
  nvgFillColor(s->vg, brake_bg);
  nvgFill(s->vg);

  nvgBeginPath(s->vg);
  nvgRect(s->vg, brake_img_x, brake_img_y, brake_img_size, brake_img_size);
  nvgFillPaint(s->vg, brake_img);
  nvgFill(s->vg);
}

static void ui_draw_vision_header(UIState *s) {
  const Rect &viz_rect = s->scene.viz_rect;
  NVGpaint gradient = nvgLinearGradient(s->vg, viz_rect.x,
                        viz_rect.y+(header_h-(header_h/2.5)),
                        viz_rect.x, viz_rect.y+header_h,
                        nvgRGBAf(0,0,0,0.45), nvgRGBAf(0,0,0,0));

  ui_draw_rect(s->vg, viz_rect.x, viz_rect.y, viz_rect.w, header_h, gradient);

  ui_draw_vision_maxspeed(s);
  ui_draw_vision_speed(s);
  ui_draw_vision_event(s);
}

//BB START: functions added for the display of various items
static int bb_ui_draw_measure(UIState *s,  const char* bb_value, const char* bb_uom, const char* bb_label,
    int bb_x, int bb_y, int bb_uom_dx,
    NVGcolor bb_valueColor, NVGcolor bb_labelColor, NVGcolor bb_uomColor,
    int bb_valueFontSize, int bb_labelFontSize, int bb_uomFontSize )  {
  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
  int dx = 0;
  if (strlen(bb_uom) > 0) {
    dx = (int)(bb_uomFontSize*2.5/2);
   }
  //print value
  nvgFontFace(s->vg, "sans-semibold");
  nvgFontSize(s->vg, bb_valueFontSize*2.5);
  nvgFillColor(s->vg, bb_valueColor);
  nvgText(s->vg, bb_x-dx/2, bb_y+ (int)(bb_valueFontSize*2.5)+5, bb_value, NULL);
  //print label
  nvgFontFace(s->vg, "sans-regular");
  nvgFontSize(s->vg, bb_labelFontSize*2.5);
  nvgFillColor(s->vg, bb_labelColor);
  nvgText(s->vg, bb_x, bb_y + (int)(bb_valueFontSize*2.5)+5 + (int)(bb_labelFontSize*2.5)+5, bb_label, NULL);
  //print uom
  if (strlen(bb_uom) > 0) {
      nvgSave(s->vg);
    int rx =bb_x + bb_uom_dx + bb_valueFontSize -3;
    int ry = bb_y + (int)(bb_valueFontSize*2.5/2)+25;
    nvgTranslate(s->vg,rx,ry);
    nvgRotate(s->vg, -1.5708); //-90deg in radians
    nvgFontFace(s->vg, "sans-regular");
    nvgFontSize(s->vg, (int)(bb_uomFontSize*2.5));
    nvgFillColor(s->vg, bb_uomColor);
    nvgText(s->vg, 0, 0, bb_uom, NULL);
    nvgRestore(s->vg);
  }
  return (int)((bb_valueFontSize + bb_labelFontSize)*2.5) + 5;
}

static void bb_ui_draw_measures_right(UIState *s, int bb_x, int bb_y, int bb_w ) {
  const UIScene *scene = &s->scene;
  int bb_rx = bb_x + (int)(bb_w/2);
  int bb_ry = bb_y;
  int bb_h = 5;
  NVGcolor lab_color = COLOR_WHITE_ALPHA(200);
  NVGcolor uom_color = COLOR_WHITE_ALPHA(200);
  int value_fontSize=30;
  int label_fontSize=15;
  int uom_fontSize = 15;
  int bb_uom_dx =  (int)(bb_w /2 - uom_fontSize*2.5) ;

   //add visual radar relative distance
  if (true) {
    char val_str[16];
    char uom_str[6];
    NVGcolor val_color = COLOR_WHITE_ALPHA(200);
    if (s->scene.lead_status) {
      //show RED if less than 5 meters
      //show orange if less than 15 meters
      if((int)(s->scene.lead_d_rel) < 15) {
        val_color = COLOR_ORANGE_ALPHA(200);
      }
      if((int)(s->scene.lead_d_rel) < 5) {
        val_color = COLOR_RED_ALPHA(200);
      }
      // lead car relative distance is always in meters
      snprintf(val_str, sizeof(val_str), "%d", (int)s->scene.lead_d_rel);
    } else {
       snprintf(val_str, sizeof(val_str), "-");
    }
    snprintf(uom_str, sizeof(uom_str), "m");
    bb_h +=bb_ui_draw_measure(s,  val_str, uom_str, "Rel Dist",
        bb_rx, bb_ry, bb_uom_dx,
        val_color, lab_color, uom_color,
        value_fontSize, label_fontSize, uom_fontSize );
    bb_ry = bb_y + bb_h;
  }

  //add visual radar relative speed
  if (true) {
    char val_str[16];
    char uom_str[6];
    NVGcolor val_color = COLOR_WHITE_ALPHA(200);
    if (s->scene.lead_status) {
      //show Orange if negative speed (approaching)
      //show Orange if negative speed faster than 5mph (approaching fast)
      if((int)(s->scene.lead_v_rel) < 0) {
        val_color = COLOR_ORANGE_ALPHA(200);
      }
      if((int)(s->scene.lead_v_rel) < -5) {
        val_color = COLOR_RED_ALPHA(200);
      }
      // lead car relative speed is always in meters
      if (s->is_metric) {
         snprintf(val_str, sizeof(val_str), "%d", (int)(s->scene.lead_v_rel * 3.6 + 0.5));
      } else {
         snprintf(val_str, sizeof(val_str), "%d", (int)(s->scene.lead_v_rel * 2.2374144 + 0.5));
      }
    } else {
       snprintf(val_str, sizeof(val_str), "-");
    }
    if (s->is_metric) {
      snprintf(uom_str, sizeof(uom_str), "km/h");;
    } else {
      snprintf(uom_str, sizeof(uom_str), "mi/h");
    }
    bb_h +=bb_ui_draw_measure(s,  val_str, uom_str, "Rel Vel",
        bb_rx, bb_ry, bb_uom_dx,
        val_color, lab_color, uom_color,
        value_fontSize, label_fontSize, uom_fontSize );
    bb_ry = bb_y + bb_h;
  }

  //add steering angle
  if (true) {
    char val_str[16];
    char uom_str[6];
    NVGcolor val_color = COLOR_GREEN_ALPHA(200);
      //show Orange if more than 30 degrees
      //show red if  more than 50 degrees
      if(((int)(s->scene.angleSteers) < -30) || ((int)(scene->angleSteers) > 30)) {
        val_color = COLOR_ORANGE_ALPHA(200);
      }
      if(((int)(s->scene.angleSteers) < -50) || ((int)(scene->angleSteers) > 50)) {
        val_color = COLOR_RED_ALPHA(200);
      }
      // steering is in degrees
      snprintf(val_str, sizeof(val_str), "%.1f",(s->scene.angleSteers));

      snprintf(uom_str, sizeof(uom_str), "");
    bb_h +=bb_ui_draw_measure(s,  val_str, uom_str, "Str Ang",
        bb_rx, bb_ry, bb_uom_dx,
        val_color, lab_color, uom_color,
        value_fontSize, label_fontSize, uom_fontSize );
    bb_ry = bb_y + bb_h;
  }

  //add desired steering angle
  if (true) {
    char val_str[16];
    char uom_str[6];
    NVGcolor val_color = COLOR_WHITE_ALPHA(200);
    if (scene->controls_state.getEnabled()) {
      //show Orange if more than 30 degrees
      //show red if  more than 50 degrees
      if(((int)(s->scene.angleSteersDes) < -30) || ((int)(scene->angleSteersDes) > 30)) {
        val_color = COLOR_WHITE_ALPHA(200);
      }
      if(((int)(s->scene.angleSteersDes) < -50) || ((int)(scene->angleSteersDes) > 50)) {
        val_color = COLOR_WHITE_ALPHA(200);
      }
      // steering is in degrees
      snprintf(val_str, sizeof(val_str), "%.1f",(s->scene.angleSteersDes));
    } else {
       snprintf(val_str, sizeof(val_str), "-");
    }
      snprintf(uom_str, sizeof(uom_str), "");
    bb_h +=bb_ui_draw_measure(s,  val_str, uom_str, "Des Str Ang",
        bb_rx, bb_ry, bb_uom_dx,
        val_color, lab_color, uom_color,
        value_fontSize, label_fontSize, uom_fontSize );
    bb_ry = bb_y + bb_h;
  }

  //add steerratio liveParameters
  if (true) {
    char val_str[16];
    char uom_str[6];
    NVGcolor val_color = COLOR_WHITE_ALPHA(200);
    if (scene->controls_state.getEnabled()) {
      snprintf(val_str, sizeof(val_str), "%.2f",(s->scene.steerRatio));
    } else {
       snprintf(val_str, sizeof(val_str), "-");
    }
      snprintf(uom_str, sizeof(uom_str), "");
    bb_h +=bb_ui_draw_measure(s,  val_str, uom_str, "Steerratio",
        bb_rx, bb_ry, bb_uom_dx,
        val_color, lab_color, uom_color,
        value_fontSize, label_fontSize, uom_fontSize );
    bb_ry = bb_y + bb_h;
  }
  
  //finally draw the frame
  bb_h += 20;
  nvgBeginPath(s->vg);
    nvgRoundedRect(s->vg, bb_x, bb_y, bb_w, bb_h, 20);
    nvgStrokeColor(s->vg, COLOR_WHITE_ALPHA(80));
    nvgStrokeWidth(s->vg, 6);
    nvgStroke(s->vg);
}

static void bb_ui_draw_UI(UIState *s)
{
  const UIScene *scene = &s->scene;

  const int bb_dmr_w = 180;
  const int bb_dmr_x = scene->viz_rect.x + scene->viz_rect.w - bb_dmr_w - (bdr_s * 2);
  const int bb_dmr_y = (scene->viz_rect.y + (bdr_s * 1.5)) + 220;

  bb_ui_draw_measures_right(s, bb_dmr_x, bb_dmr_y-20, bb_dmr_w);
}

//BB END: functions added for the display of various items

static void ui_draw_vision_footer(UIState *s) {
  ui_draw_vision_face(s);
  ui_draw_vision_brake(s);
  bb_ui_draw_UI(s);
}

void ui_draw_vision_alert(UIState *s, cereal::ControlsState::AlertSize va_size, int va_color,
                          const char* va_text1, const char* va_text2) {
  static std::map<cereal::ControlsState::AlertSize, const int> alert_size_map = {
      {cereal::ControlsState::AlertSize::NONE, 0},
      {cereal::ControlsState::AlertSize::SMALL, 241},
      {cereal::ControlsState::AlertSize::MID, 390},
      {cereal::ControlsState::AlertSize::FULL, s->fb_h}};

  const UIScene *scene = &s->scene;
  bool longAlert1 = strlen(va_text1) > 15;

  const uint8_t *color = alert_colors[va_color];
  int alr_s = alert_size_map[va_size];

  const int alr_x = scene->viz_rect.x - bdr_s;
  const int alr_w = scene->viz_rect.w + (bdr_s*2);
  const int alr_h = alr_s+(va_size==cereal::ControlsState::AlertSize::NONE?0:bdr_s);
  const int alr_y = s->fb_h-alr_h;

  ui_draw_rect(s->vg, alr_x, alr_y, alr_w, alr_h, nvgRGBA(color[0],color[1],color[2],(color[3]*s->alert_blinking_alpha)));

  NVGpaint gradient = nvgLinearGradient(s->vg, alr_x, alr_y, alr_x, alr_y+alr_h,
                        nvgRGBAf(0.0,0.0,0.0,0.05), nvgRGBAf(0.0,0.0,0.0,0.35));
  ui_draw_rect(s->vg, alr_x, alr_y, alr_w, alr_h, gradient);

  nvgFillColor(s->vg, COLOR_WHITE);
  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

  if (va_size == cereal::ControlsState::AlertSize::SMALL) {
    ui_draw_text(s->vg, alr_x+alr_w/2, alr_y+alr_h/2+15, va_text1, 40*2.5, COLOR_WHITE, s->font_sans_semibold);
  } else if (va_size == cereal::ControlsState::AlertSize::MID) {
    ui_draw_text(s->vg, alr_x+alr_w/2, alr_y+alr_h/2-45, va_text1, 48*2.5, COLOR_WHITE, s->font_sans_bold);
    ui_draw_text(s->vg, alr_x+alr_w/2, alr_y+alr_h/2+75, va_text2, 36*2.5, COLOR_WHITE, s->font_sans_regular);
  } else if (va_size == cereal::ControlsState::AlertSize::FULL) {
    nvgFontSize(s->vg, (longAlert1?72:96)*2.5);
    nvgFontFaceId(s->vg, s->font_sans_bold);
    nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgTextBox(s->vg, alr_x, alr_y+(longAlert1?360:420), alr_w-60, va_text1, NULL);
    nvgFontSize(s->vg, 48*2.5);
    nvgFontFaceId(s->vg,  s->font_sans_regular);
    nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
    nvgTextBox(s->vg, alr_x, alr_h-(longAlert1?300:360), alr_w-60, va_text2, NULL);
  }
}

static void ui_draw_vision(UIState *s) {
  const UIScene *scene = &s->scene;
  const Rect &viz_rect = scene->viz_rect;
  // Draw video frames
  glEnable(GL_SCISSOR_TEST);
  glViewport(viz_rect.x+scene->ui_viz_ro, viz_rect.y, s->fb_w - bdr_s*2, viz_rect.h);
  glScissor(viz_rect.x, viz_rect.y, viz_rect.w, viz_rect.h);
  draw_frame(s);
  glDisable(GL_SCISSOR_TEST);

  glViewport(0, 0, s->fb_w, s->fb_h);

  // Draw augmented elements
  if (!scene->frontview && scene->world_objects_visible) {
    ui_draw_world(s);
  }
  // Set Speed, Current Speed, Status/Events
  if (!scene->frontview) {
    ui_draw_vision_header(s);
  } else {
    ui_draw_driver_view(s);
  }

  if (scene->alert_size != cereal::ControlsState::AlertSize::NONE) {
    ui_draw_vision_alert(s, scene->alert_size, s->status,
                         scene->alert_text1.c_str(), scene->alert_text2.c_str());
  } else if (!scene->frontview) {
    ui_draw_vision_footer(s);
  }
}

static void ui_draw_background(UIState *s) {
  const Color color = bg_colors[s->status];
  glClearColor(color.r/256.0, color.g/256.0, color.b/256.0, 1.0);
  glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
}

void ui_draw(UIState *s) {
  s->scene.viz_rect = Rect{bdr_s * 3, bdr_s, s->fb_w - 4 * bdr_s, s->fb_h - 2 * bdr_s};
  s->scene.ui_viz_ro = 0;
  if (!s->scene.uilayout_sidebarcollapsed) {
    s->scene.viz_rect.x = sbr_w + bdr_s;
    s->scene.viz_rect.w = s->fb_w - s->scene.viz_rect.x - bdr_s;
    s->scene.ui_viz_ro = -(sbr_w - 6 * bdr_s);
  }

  ui_draw_background(s);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glViewport(0, 0, s->fb_w, s->fb_h);
  nvgBeginFrame(s->vg, s->fb_w, s->fb_h, 1.0f);
  ui_draw_sidebar(s);
  if (s->started && s->active_app == cereal::UiLayoutState::App::NONE &&
      s->status != STATUS_OFFROAD && s->vision_connected) {
    ui_draw_vision(s);
  }
  nvgEndFrame(s->vg);
  glDisable(GL_BLEND);
}

void ui_draw_image(NVGcontext *vg, float x, float y, float w, float h, int image, float alpha){
  nvgBeginPath(vg);
  NVGpaint imgPaint = nvgImagePattern(vg, x, y, w, h, 0, image, alpha);
  nvgRect(vg, x, y, w, h);
  nvgFillPaint(vg, imgPaint);
  nvgFill(vg);
}

void ui_draw_rect(NVGcontext *vg, float x, float y, float w, float h, NVGcolor color, float r, int width) {
  nvgBeginPath(vg);
  r > 0? nvgRoundedRect(vg, x, y, w, h, r) : nvgRect(vg, x, y, w, h);
  if (width) {
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
  } else {
    nvgFillColor(vg, color);
    nvgFill(vg);
  }
}

void ui_draw_rect(NVGcontext *vg, float x, float y, float w, float h, NVGpaint &paint, float r){
  nvgBeginPath(vg);
  r > 0? nvgRoundedRect(vg, x, y, w, h, r) : nvgRect(vg, x, y, w, h);
  nvgFillPaint(vg, paint);
  nvgFill(vg);
}

static const char frame_vertex_shader[] =
#ifdef NANOVG_GL3_IMPLEMENTATION
  "#version 150 core\n"
#else
  "#version 300 es\n"
#endif
  "in vec4 aPosition;\n"
  "in vec4 aTexCoord;\n"
  "uniform mat4 uTransform;\n"
  "out vec4 vTexCoord;\n"
  "void main() {\n"
  "  gl_Position = uTransform * aPosition;\n"
  "  vTexCoord = aTexCoord;\n"
  "}\n";

static const char frame_fragment_shader[] =
#ifdef NANOVG_GL3_IMPLEMENTATION
  "#version 150 core\n"
#else
  "#version 300 es\n"
#endif
  "precision mediump float;\n"
  "uniform sampler2D uTexture;\n"
  "in vec4 vTexCoord;\n"
  "out vec4 colorOut;\n"
  "void main() {\n"
  "  colorOut = texture(uTexture, vTexCoord.xy);\n"
  "}\n";

static const mat4 device_transform = {{
  1.0,  0.0, 0.0, 0.0,
  0.0,  1.0, 0.0, 0.0,
  0.0,  0.0, 1.0, 0.0,
  0.0,  0.0, 0.0, 1.0,
}};

// frame from 4/3 to 16/9 display
static const mat4 full_to_wide_frame_transform = {{
  .75,  0.0, 0.0, 0.0,
  0.0,  1.0, 0.0, 0.0,
  0.0,  0.0, 1.0, 0.0,
  0.0,  0.0, 0.0, 1.0,
}};

void ui_nvg_init(UIState *s) {
  // init drawing
#ifdef QCOM
  // on QCOM, we enable MSAA
  s->vg = nvgCreate(0);
#else
  s->vg = nvgCreate(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
#endif

  assert(s->vg);

  s->font_sans_regular = nvgCreateFont(s->vg, "sans-regular", "../assets/fonts/opensans_regular.ttf");
  assert(s->font_sans_regular >= 0);
  s->font_sans_semibold = nvgCreateFont(s->vg, "sans-semibold", "../assets/fonts/opensans_semibold.ttf");
  assert(s->font_sans_semibold >= 0);
  s->font_sans_bold = nvgCreateFont(s->vg, "sans-bold", "../assets/fonts/opensans_bold.ttf");
  assert(s->font_sans_bold >= 0);

  s->img_wheel = nvgCreateImage(s->vg, "../assets/img_chffr_wheel.png", 1);
  assert(s->img_wheel != 0);
  s->img_turn = nvgCreateImage(s->vg, "../assets/img_trafficSign_turn.png", 1);
  assert(s->img_turn != 0);
  s->img_face = nvgCreateImage(s->vg, "../assets/img_driver_face.png", 1);
  assert(s->img_face != 0);
  s->img_brake = nvgCreateImage(s->vg, "../assets/img_brake_disc.png", 1);
  assert(s->img_brake >= 0);
  s->img_button_settings = nvgCreateImage(s->vg, "../assets/images/button_settings.png", 1);
  assert(s->img_button_settings != 0);
  s->img_button_home = nvgCreateImage(s->vg, "../assets/images/button_home.png", 1);
  assert(s->img_button_home != 0);
  s->img_battery = nvgCreateImage(s->vg, "../assets/images/battery.png", 1);
  assert(s->img_battery != 0);
  s->img_battery_charging = nvgCreateImage(s->vg, "../assets/images/battery_charging.png", 1);
  assert(s->img_battery_charging != 0);

  for(int i=0;i<=5;++i) {
    char network_asset[32];
    snprintf(network_asset, sizeof(network_asset), "../assets/images/network_%d.png", i);
    s->img_network[i] = nvgCreateImage(s->vg, network_asset, 1);
    assert(s->img_network[i] != 0);
  }

  // init gl
  s->frame_program = load_program(frame_vertex_shader, frame_fragment_shader);
  assert(s->frame_program);

  s->frame_pos_loc = glGetAttribLocation(s->frame_program, "aPosition");
  s->frame_texcoord_loc = glGetAttribLocation(s->frame_program, "aTexCoord");

  s->frame_texture_loc = glGetUniformLocation(s->frame_program, "uTexture");
  s->frame_transform_loc = glGetUniformLocation(s->frame_program, "uTransform");

  glViewport(0, 0, s->fb_w, s->fb_h);

  glDisable(GL_DEPTH_TEST);

  assert(glGetError() == GL_NO_ERROR);

  for(int i = 0; i < 2; i++) {
    float x1, x2, y1, y2;
    if (i == 1) {
      // flip horizontally so it looks like a mirror
      x1 = 0.0;
      x2 = 1.0;
      y1 = 1.0;
      y2 = 0.0;
    } else {
      x1 = 1.0;
      x2 = 0.0;
      y1 = 1.0;
      y2 = 0.0;
    }
    const uint8_t frame_indicies[] = {0, 1, 2, 0, 2, 3};
    const float frame_coords[4][4] = {
      {-1.0, -1.0, x2, y1}, //bl
      {-1.0,  1.0, x2, y2}, //tl
      { 1.0,  1.0, x1, y2}, //tr
      { 1.0, -1.0, x1, y1}, //br
    };

    glGenVertexArrays(1, &s->frame_vao[i]);
    glBindVertexArray(s->frame_vao[i]);
    glGenBuffers(1, &s->frame_vbo[i]);
    glBindBuffer(GL_ARRAY_BUFFER, s->frame_vbo[i]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(frame_coords), frame_coords, GL_STATIC_DRAW);
    glEnableVertexAttribArray(s->frame_pos_loc);
    glVertexAttribPointer(s->frame_pos_loc, 2, GL_FLOAT, GL_FALSE,
                          sizeof(frame_coords[0]), (const void *)0);
    glEnableVertexAttribArray(s->frame_texcoord_loc);
    glVertexAttribPointer(s->frame_texcoord_loc, 2, GL_FLOAT, GL_FALSE,
                          sizeof(frame_coords[0]), (const void *)(sizeof(float) * 2));
    glGenBuffers(1, &s->frame_ibo[i]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->frame_ibo[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(frame_indicies), frame_indicies, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glBindVertexArray(0);
  }

  // frame from 4/3 to box size with a 2x zoom
  const mat4 frame_transform = {{
    (float)(2*(4./3.)/((float)(s->fb_w-(bdr_s*2))/(s->fb_h-(bdr_s*2)))), 0.0, 0.0, 0.0,
    0.0, 2.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
  }};

  s->front_frame_mat = matmul(device_transform, full_to_wide_frame_transform);
  s->rear_frame_mat = matmul(device_transform, frame_transform);

  for(int i = 0; i < UI_BUF_COUNT; i++) {
    s->khr[i] = 0;
    s->priv_hnds[i] = NULL;
  }
}
