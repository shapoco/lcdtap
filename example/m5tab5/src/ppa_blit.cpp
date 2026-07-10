#include "lcdtap/m5tab5/ppa_blit.hpp"

#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"

namespace lcdtap::m5tab5 {

namespace {
constexpr const char *TAG = "ppa_blit";

// This app only ever runs one PpaBlitState with at most 2 transactions in
// flight (matching DisplayOutState's 2 ping-pong strip buffers), so a
// fixed 2-slot table -- indexed by a counter that alternates in lockstep
// with the caller's own ping-pong index -- is enough to recover, in
// onTransDone(), which PpaBlitState/semaphore/submit-time a completion
// belongs to (ppa_event_callback_t only carries one per-transaction
// void* of context).
struct PendingOp {
  PpaBlitState *state = nullptr;
  SemaphoreHandle_t sem = nullptr;
  int64_t submitUs = 0;
};
PendingOp gPending[2];
int gNextSlot = 0;

bool onTransDone(ppa_client_handle_t, ppa_event_data_t *, void *userData) {
  auto *p = (PendingOp *)userData;
  p->state->busyUs += esp_timer_get_time() - p->submitUs;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(p->sem, &woken);
  return woken == pdTRUE;
}
}  // namespace

bool ppaBlitInit(PpaBlitState *s, M5GFX *gfx) {
  auto *panel = static_cast<lgfx::Panel_DSI *>(gfx->getPanel());
  if (!panel) {
    ESP_LOGE(TAG, "no panel");
    return false;
  }

  s->panelW = panel->config().panel_width;
  s->panelH = panel->config().panel_height;
  s->panelFb = panel->config_detail().buffer;
  if (s->panelW != 720 || s->panelH != 1280 || !s->panelFb) {
    ESP_LOGE(TAG, "unexpected panel geometry %ux%u fb=%p", (unsigned)s->panelW,
             (unsigned)s->panelH, s->panelFb);
    s->ok = false;
    return false;
  }

  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 2;
  ppa_client_handle_t client = nullptr;
  if (ppa_register_client(&cfg, &client) != ESP_OK) {
    ESP_LOGE(TAG, "ppa_register_client failed");
    s->ok = false;
    return false;
  }

  ppa_event_callbacks_t cbs = {};
  cbs.on_trans_done = onTransDone;
  ppa_client_register_event_callbacks(client, &cbs);

  s->client = client;
  s->ok = true;
  return true;
}

bool ppaBlitSubmitStripAsync(PpaBlitState *s, uint16_t logicalY,
                             uint16_t numLines, uint16_t logicalW,
                             const uint16_t *stripBuf,
                             SemaphoreHandle_t doneSem) {
  if (!s->ok) return false;

  // The strip buffer was written by the CPU; PPA reads it as a DMA bus
  // master, which bypasses the CPU data cache, so the write-back must
  // happen before the transaction is submitted.
  size_t bytes = (size_t)logicalW * numLines * sizeof(uint16_t);
  esp_cache_msync(
      (void *)stripBuf, bytes,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

  int slot = gNextSlot;
  gNextSlot ^= 1;
  gPending[slot] = {s, doneSem, esp_timer_get_time()};

  ppa_srm_oper_config_t op = {};
  op.in.buffer = stripBuf;
  op.in.pic_w = logicalW;
  op.in.pic_h = numLines;
  op.in.block_w = logicalW;
  op.in.block_h = numLines;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

  op.out.buffer = s->panelFb;
  op.out.buffer_size = (uint32_t)s->panelW * s->panelH * sizeof(uint16_t);
  op.out.pic_w = s->panelW;
  op.out.pic_h = s->panelH;
  // No rotation: the strip's logical Y is already the physical row offset.
  op.out.block_offset_x = 0;
  op.out.block_offset_y = logicalY;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.mirror_x = false;
  op.mirror_y = false;
  op.mode = PPA_TRANS_MODE_NON_BLOCKING;
  op.user_data = &gPending[slot];

  esp_err_t err =
      ppa_do_scale_rotate_mirror((ppa_client_handle_t)s->client, &op);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ppa_do_scale_rotate_mirror failed: %d", (int)err);
    return false;
  }
  return true;
}

}  // namespace lcdtap::m5tab5
