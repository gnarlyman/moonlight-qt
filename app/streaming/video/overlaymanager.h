#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    // Fork addition: a small, quiet indicator of which input path is live.
    // Its own slot, so it never contends with the two above.
    OverlayKvmInput,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);

    /**
     * Show text, then hide it automatically after durationMs.
     *
     * updateOverlayExpiry() must be called regularly from the same thread that
     * owns rendering (the main thread) for the hide to happen. The overlay is
     * hidden by expiry only; showing it again restarts the timer.
     */
    void showOverlayTextTimed(OverlayType type, const char* text, Uint32 durationMs);

    /** Hide any timed overlay whose time is up. Safe and cheap to call often. */
    void updateOverlayExpiry(Uint32 nowMs);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled;
        Uint32 expiry; // 0 = no pending auto-hide
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
