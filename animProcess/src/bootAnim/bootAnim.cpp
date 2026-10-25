/**
 * File: bootAnim.cpp
 *
 * Author: Al Chaussee
 * Created: 07/27/2018
 *
 * Description: Draws the boot animation to the screen forever until stopped.
 *              Leaves the screen on when stopped under the assumption whatever is stopping
 *              it will continue to use the screen.
 *
 *              Default animation is a procedural warp-speed starfield — stars rush outward
 *              from center with motion-blur trails, giving Vector a sci-fi explorer character
 *              on every boot. Can be overridden by placing a raw RGB565 animation file at
 *              /persist/boot_anim.raw.
 *
 * Copyright: Anki, Inc. 2018
 *
 **/

#include "core/lcd.h"

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <signal.h>
#include <sys/stat.h>
#include <vector>

#define FRAME_WIDTH_SANTEK  184
#define FRAME_HEIGHT_SANTEK 96
#define FRAME_WIDTH_MIDAS   160
#define FRAME_HEIGHT_MIDAS  80
#define NUM_BYTES_PER_FRAME_SANTEK (FRAME_WIDTH_SANTEK * FRAME_HEIGHT_SANTEK * sizeof(uint16_t))
#define NUM_BYTES_PER_FRAME_MIDAS  (FRAME_WIDTH_MIDAS  * FRAME_HEIGHT_MIDAS  * sizeof(uint16_t))

namespace {
  static const char* kAnimPathCustom = "/persist/boot_anim.raw";
  bool gShutdown = false;
}

extern "C" void core_common_on_exit(void)
{
  lcd_shutdown();
}

bool use_santek_sizes()
{
  return lcd_display_version() == SANTEK;
}

static void animate(void* frame)
{
  if (use_santek_sizes()) {
    lcd_draw_frame2((uint16_t*)frame, NUM_BYTES_PER_FRAME_SANTEK);
  } else {
    lcd_draw_frame2((uint16_t*)frame, NUM_BYTES_PER_FRAME_MIDAS);
  }
}

static void handler(int signum)
{
  gShutdown = true;
}

static bool custom_exists()
{
  struct stat buffer;
  return (stat(kAnimPathCustom, &buffer) == 0);
}

// ----------------------------------------------------------------------------
// Procedural warp-speed starfield boot animation
//
// Stars are projected through a perspective divide (x/z, y/z) so they appear
// to rush outward from the center as z decreases toward zero.  Instead of
// clearing the framebuffer each tick, we dim every pixel slightly — this
// creates automatic motion-blur trails whose length is proportional to a
// star's on-screen speed (close/fast stars leave long streaks; far/slow ones
// vanish immediately).  Color shifts from deep blue when distant to warm white
// when close, reinforcing the sense of depth.
//
// The seed is fixed so the exact same star field appears on every boot —
// intentional, not random-looking.
// ----------------------------------------------------------------------------
static void run_procedural_boot(int width, int height)
{
  struct Star { float x, y, z; };

  const int N_STARS = 110;
  Star stars[N_STARS];

  // Deterministic LCG — same field every boot
  unsigned seed = 0xc0ffee42u;
  auto rnd = [&](float lo, float hi) -> float {
    seed = seed * 1664525u + 1013904223u;
    return lo + (float)((seed >> 8) & 0xFFFFu) / 65535.0f * (hi - lo);
  };

  for (auto& s : stars) {
    s.x = rnd(-1.0f, 1.0f);
    s.y = rnd(-1.0f, 1.0f);
    s.z = rnd(0.05f, 1.0f);
  }

  std::vector<uint16_t> fb(width * height, 0);
  const float cx    = width  * 0.5f;
  const float cy    = height * 0.5f;
  const float SPEED = 0.020f;   // z units consumed per frame (~30 fps → full traversal ≈1.6s)

  while (!gShutdown) {
    // Dim every pixel — near-black trail fades fast, bright trail fades slowly,
    // giving automatic streak lengths proportional to star speed.
    for (auto& px : fb) {
      const uint8_t r = (px >> 11) & 0x1Fu;
      const uint8_t g = (px >>  5) & 0x3Fu;
      const uint8_t b =  px        & 0x1Fu;
      px = (uint16_t)(((r > 3u ? r - 3u : 0u) << 11) |
                      ((g > 5u ? g - 5u : 0u) <<  5) |
                       (b > 3u ? b - 3u : 0u));
    }

    for (auto& s : stars) {
      s.z -= SPEED;

      if (s.z <= 0.01f) {
        // Respawn far away — sprinkle across the whole depth range so the
        // screen never empties all at once
        s.x = rnd(-1.0f, 1.0f);
        s.y = rnd(-1.0f, 1.0f);
        s.z = rnd(0.85f, 1.0f);
        continue;
      }

      // Perspective projection: star appears farther from center as z shrinks
      const float sx = cx + s.x / s.z * cx;
      const float sy = cy + s.y / s.z * cy;
      if (sx < 0.0f || sx >= (float)width || sy < 0.0f || sy >= (float)height) continue;

      // t: 0 = far/dim, 1 = close/bright
      const float t = 1.0f - s.z;

      // Deep blue → blue-white → warm white as the star approaches
      const uint8_t rv = (uint8_t)(t * t * 31.0f);
      const uint8_t gv = (uint8_t)(t * 0.82f * 63.0f);
      const uint8_t bv = (uint8_t)(std::min(1.0f, t * 1.5f + 0.12f) * 31.0f);

      fb[(int)sy * width + (int)sx] = (uint16_t)((rv << 11) | (gv << 5) | bv);
    }

    lcd_draw_frame2(fb.data(), (uint32_t)(width * height * sizeof(uint16_t)));
    // lcd_draw_frame2 is a blocking call (~41ms on Santek); no extra sleep needed
  }
}

// ----------------------------------------------------------------------------

int main(int argc, char** argv)
{
  signal(SIGTERM, handler);
  signal(SIGINT,  handler);

  lcd_init();
  lcd_clear_screen();
  lcd_shutdown();

  int rc = lcd_init();
  if (rc != 0) {
    printf("Failed to init lcd\n");
    return rc;
  }

  const int width  = use_santek_sizes() ? FRAME_WIDTH_SANTEK  : FRAME_WIDTH_MIDAS;
  const int height = use_santek_sizes() ? FRAME_HEIGHT_SANTEK : FRAME_HEIGHT_MIDAS;

  if (!custom_exists()) {
    // No user override — run the built-in warp-speed starfield
    run_procedural_boot(width, height);
    lcd_shutdown();
    return 0;
  }

  // User has placed a custom animation at /persist/boot_anim.raw — play it
  int fd = open(kAnimPathCustom, O_RDONLY, 0644);
  if (fd < 0) {
    printf("Failed to open %s\n", kAnimPathCustom);
    return -1;
  }

  const off_t len = lseek(fd, 0, SEEK_END);
  if (len == -1) {
    printf("lseek failed\n");
    return -1;
  }

  const uint32_t frame_bytes    = use_santek_sizes() ? NUM_BYTES_PER_FRAME_SANTEK : NUM_BYTES_PER_FRAME_MIDAS;
  const uint32_t kTotalNumFrames = (uint32_t)len / frame_bytes;

  void* anim = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
  if (anim == nullptr) {
    printf("mmap failed\n");
    return -1;
  }

  rc = close(fd);
  if (rc != 0) {
    printf("close failed\n");
  }

  // Keep track of elapsed time to skip frames and maintain ~24 fps
  uint32_t timeCount = 0;

  while (!gShutdown) {
    static const uint32_t kFrameDuration_ms = 41;
    const uint32_t nextFrameToDraw = timeCount / kFrameDuration_ms;

    const auto startTime = std::chrono::steady_clock::now();
    animate(((uint8_t*)anim) + (nextFrameToDraw * frame_bytes));
    const auto endTime = std::chrono::steady_clock::now();

    timeCount += (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (timeCount >= (kTotalNumFrames - 1) * kFrameDuration_ms) {
      timeCount %= kTotalNumFrames * kFrameDuration_ms;
    }
  }

  rc = munmap(anim, len);
  if (rc != 0) {
    printf("munmap failed\n");
  }

  lcd_shutdown();
  return 0;
}
