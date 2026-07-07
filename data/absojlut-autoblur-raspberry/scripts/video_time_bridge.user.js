// ==UserScript==
// @name         AbsoJlut AutoBlur Raspberry Video Bridge + Player Overlay
// @namespace    movie-blur-sync
// @version      3.0
// @description  Sends HTML5 video.currentTime to local AbsoJlut AutoBlur Raspberry app and shows title/timer overlay attached to the stable video root, not hidden with player controls. Better title filtering. OBS Dock app v3.0 compatible.
// @match        *://*/*
// @run-at       document-idle
// @grant        GM_xmlhttpRequest
// @grant        GM.xmlHttpRequest
// @connect      127.0.0.1
// @connect      localhost
// ==/UserScript==

(function () {
    'use strict';

    const BRIDGE_URL = 'http://127.0.0.1:8799/push';
    const SEND_EVERY_MS = 300;
    const DEBUG = true;

    const OVERLAY_ENABLED = true;
    const OVERLAY_HIDE_AFTER_MS = 12000;
    const CONTROL_LAYER_RE = /control|controls|bar|toolbar|timeline|progress|volume|button|settings|quality|subtitle|caption|menu|panel|poster|overlay|ui|bottom|top|hover|tooltip|cursor|ad|preroll/i;
    const PLAYER_LAYER_RE = /player|video|iframe|responsive|alloha|kodik|cdn|movie|media|vjs|jw|plyr|html5|stream|embed|container|wrapper|root/i;
    const STORAGE_PREFIX = 'movieBlurSyncOverlay.';
    const PLAYER_PADDING_X = 22;
    const PLAYER_PADDING_Y = 18;

    let lastSentAt = 0;
    let lastLogAt = 0;
    let lastWarnNoGrantAt = 0;
    let lastVideo = null;
    let lastPayload = null;
    let lastTopTitle = '';
    let lastTitleBroadcastAt = 0;

    let overlayEl = null;
    let overlayTitleEl = null;
    let overlayTimeEl = null;
    let overlayHostEl = null;
    let overlayHidden = localStorage.getItem(STORAGE_PREFIX + 'hidden') === '1';

    function log(...args) {
        if (!DEBUG) return;
        const now = Date.now();
        if (now - lastLogAt < 1500) return;
        lastLogAt = now;
        console.log('[MovieBlurSync]', ...args);
    }

    function warnNoGrant() {
        const now = Date.now();
        if (now - lastWarnNoGrantAt < 5000) return;
        lastWarnNoGrantAt = now;
        console.warn('[MovieBlurSync] GM_xmlhttpRequest недоступен. Не использую fetch, потому что браузер блокирует 127.0.0.1 через CORS/PNA. Удали старый скрипт/букмарклет и установи актуальный video_time_bridge.user.js в Tampermonkey/Violentmonkey.');
    }

    function videoScore(video) {
        const rect = video.getBoundingClientRect ? video.getBoundingClientRect() : { width: 0, height: 0 };
        const area = Math.max(0, rect.width || 0) * Math.max(0, rect.height || 0);
        const duration = Number.isFinite(video.duration) ? video.duration : 0;
        const playing = !video.paused && !video.ended ? 1 : 0;
        const loaded = video.readyState || 0;
        return playing * 100000000 + duration * 1000 + area + loaded;
    }

    function pickVideoInDocument(doc) {
        try {
            const videos = Array.from(doc.querySelectorAll('video')).filter((v) => Number.isFinite(v.currentTime));
            if (!videos.length) return null;
            videos.sort((a, b) => videoScore(b) - videoScore(a));
            return videos[0];
        } catch (_e) {
            return null;
        }
    }

    function pickVideoFromSameOriginFrames(win, depth = 0) {
        if (!win || depth > 6) return null;

        let best = null;
        try {
            best = pickVideoInDocument(win.document);
        } catch (_e) {}

        try {
            const frames = Array.from(win.frames || []);
            for (const frame of frames) {
                let candidate = null;
                try {
                    candidate = pickVideoFromSameOriginFrames(frame, depth + 1);
                } catch (_e) {
                    candidate = null;
                }
                if (candidate && (!best || videoScore(candidate) > videoScore(best))) {
                    best = candidate;
                }
            }
        } catch (_e) {}

        return best;
    }

    function pickVideo() {
        // Внутри iframe чаще всего видео лежит в текущем document. Это важно для оверлея:
        // рисуем таймер именно в том фрейме, где находится video, тогда нет лага при скролле.
        let video = pickVideoInDocument(document);
        if (!video) video = pickVideoFromSameOriginFrames(window);
        if (video) lastVideo = video;
        return video || lastVideo;
    }

    function gmRequest(details) {
        if (typeof GM_xmlhttpRequest === 'function') {
            GM_xmlhttpRequest(details);
            return true;
        }
        if (typeof GM !== 'undefined' && GM && typeof GM.xmlHttpRequest === 'function') {
            try {
                GM.xmlHttpRequest(details);
                return true;
            } catch (e) {
                console.warn('[MovieBlurSync] GM.xmlHttpRequest error', e);
                return false;
            }
        }
        return false;
    }

    function sendPayloadToApp(payload) {
        const ok = gmRequest({
            method: 'POST',
            url: BRIDGE_URL,
            data: JSON.stringify(payload),
            headers: { 'Content-Type': 'application/json' },
            timeout: 1500,
            onload: function (res) {
                if (res && res.status >= 200 && res.status < 300) {
                    log('sent via GM', Number(payload.currentTime || 0).toFixed(2), payload.frame, payload.title || payload.url);
                } else {
                    console.warn('[MovieBlurSync] bridge HTTP status', res && res.status, res && res.responseText);
                }
            },
            onerror: function (err) { console.warn('[MovieBlurSync] send error', err); },
            ontimeout: function () { console.warn('[MovieBlurSync] send timeout'); }
        });
        if (!ok) warnNoGrant();
    }

    function readText(selector) {
        try {
            const el = document.querySelector(selector);
            const text = el ? String(el.textContent || '').trim() : '';
            return text || '';
        } catch (_e) {
            return '';
        }
    }

    const BAD_TITLE_RE = /^(скопировать|copy|копировать|плеер|player|серия|сезон|season|episode|сервер|server|trailer|трейлер|настройки|settings|качество|quality|subtitles?|субтитры|fullscreen|полный экран|play|pause|movie blur sync)$/i;
    const BAD_TITLE_PART_RE = /^(скопировать|copy)$/i;

    function cleanTitle(value) {
        let title = String(value || '').replace(/\s+/g, ' ').trim();
        title = title
            .replace(/\s*[—|-]\s*(Raspberry|ReYohoho|ReYohoho\.space|смотреть онлайн|онлайн|плеер).*$/i, '')
            .replace(/^Raspberry\s*[—|-]\s*/i, '')
            .replace(/^(Смотреть|Смотреть фильм|Фильм)\s+/i, '')
            .trim();
        return title;
    }

    function isGoodTitle(value) {
        const title = cleanTitle(value);
        if (!title || title.length < 2 || title.length > 120) return false;
        if (/^https?:\/\//i.test(title)) return false;
        if (BAD_TITLE_RE.test(title)) return false;
        if (BAD_TITLE_PART_RE.test(title.replace(/[.:!—\-\s]+$/g, '').trim())) return false;
        if (/^\d{1,2}:\d{2}(?::\d{2})?(\s*\/\s*\d{1,2}:\d{2}(?::\d{2})?)?$/.test(title)) return false;
        return true;
    }

    function readMetaContent(selector) {
        try {
            const el = document.querySelector(selector);
            return el ? String(el.getAttribute('content') || '').trim() : '';
        } catch (_e) {
            return '';
        }
    }

    function getPageTitle() {
        // Не берём слишком общий [class*=title]: у Raspberry/плееров туда часто попадают кнопки
        // вроде «Скопировать», «Плеер», «Сезон 1». Сначала используем нормальные источники.
        const candidates = [
            readMetaContent('meta[property="og:title"]'),
            readMetaContent('meta[name="twitter:title"]'),
            document.title,
            readText('h1'),
            readText('[itemprop="name"]'),
            readText('[data-testid*="movie" i][data-testid*="title" i]'),
            readText('[data-testid*="film" i][data-testid*="title" i]'),
            readText('[class*="movie-title" i]'),
            readText('[class*="film-title" i]'),
            readText('[class*="kinopoisk-title" i]')
        ];
        for (const candidate of candidates) {
            const title = cleanTitle(candidate);
            if (isGoodTitle(title)) return title;
        }
        return '';
    }

    function getBestTitle(payload) {
        const candidates = [
            lastTopTitle,
            getPageTitle(),
            payload && payload.title,
            document.title
        ];
        for (const candidate of candidates) {
            const title = cleanTitle(candidate);
            if (isGoodTitle(title)) return title;
        }
        return 'Фильм';
    }

    function broadcastTitleToChildFrames(force = false) {
        if (window.top !== window) return;
        const now = Date.now();
        if (!force && now - lastTitleBroadcastAt < 1000) return;
        lastTitleBroadcastAt = now;

        const title = getPageTitle() || (isGoodTitle(document.title) ? cleanTitle(document.title) : '');
        if (isGoodTitle(title)) lastTopTitle = title;
        const msg = { __movieBlurSyncTitle: true, title: isGoodTitle(title) ? title : '' };

        function sendToFrames(win, depth = 0) {
            if (!win || depth > 6) return;
            let frames = [];
            try { frames = Array.from(win.frames || []); } catch (_e) { return; }
            for (const frame of frames) {
                try { frame.postMessage(msg, '*'); } catch (_e) {}
                try { sendToFrames(frame, depth + 1); } catch (_e) {}
            }
        }

        sendToFrames(window);
    }

    function forwardTitleToChildren(title) {
        const msg = { __movieBlurSyncTitle: true, title };
        let frames = [];
        try { frames = Array.from(window.frames || []); } catch (_e) { return; }
        for (const frame of frames) {
            try { frame.postMessage(msg, '*'); } catch (_e) {}
        }
    }

    function formatTime(seconds, duration) {
        if (!Number.isFinite(seconds) || seconds < 0) seconds = 0;
        const total = Math.floor(seconds);
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const forceHours = Number.isFinite(duration) && duration >= 3600;
        if (h > 0 || forceHours) {
            return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
        }
        return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
    }

    function rectToTopViewport(el) {
        try {
            const rect = el.getBoundingClientRect();
            let left = rect.left;
            let top = rect.top;
            const width = rect.width;
            const height = rect.height;
            let win = el.ownerDocument && el.ownerDocument.defaultView;
            let ok = true;

            while (win && win !== win.top) {
                const frame = win.frameElement;
                if (!frame || !frame.getBoundingClientRect) {
                    ok = false;
                    break;
                }
                const fr = frame.getBoundingClientRect();
                left += fr.left;
                top += fr.top;
                win = win.parent;
            }
            return { left, top, width, height, ok: Boolean(ok && win === win.top) };
        } catch (_e) {
            return null;
        }
    }

    function ensureStyle() {
        if (document.getElementById('movie-blur-sync-overlay-style')) return;
        const style = document.createElement('style');
        style.id = 'movie-blur-sync-overlay-style';
        style.textContent = `
            #movie-blur-sync-overlay {
                position: absolute;
                left: ${PLAYER_PADDING_X}px;
                top: ${PLAYER_PADDING_Y}px;
                z-index: 2147483647;
                pointer-events: none;
                user-select: none;
                display: block !important;
                opacity: 1 !important;
                visibility: visible !important;
                font-family: Inter, Segoe UI, Arial, sans-serif;
                color: #f3f5fb;
                max-width: calc(100% - ${PLAYER_PADDING_X * 2}px);
                padding: 0;
                margin: 0;
                background: transparent !important;
                border: 0 !important;
                box-shadow: none !important;
                backdrop-filter: none !important;
                -webkit-backdrop-filter: none !important;
                transform: translateZ(0);
            }
            #movie-blur-sync-overlay.mbs-fixed {
                position: fixed;
                left: ${PLAYER_PADDING_X}px;
                top: ${PLAYER_PADDING_Y}px;
                max-width: calc(100vw - ${PLAYER_PADDING_X * 2}px);
            }
            #movie-blur-sync-overlay.is-hidden { display: none !important; }
            #movie-blur-sync-overlay .mbs-title {
                font-size: clamp(24px, 2.7vw, 42px);
                line-height: 1.05;
                font-weight: 900;
                letter-spacing: .01em;
                color: rgba(245, 247, 255, .92);
                text-shadow: 0 2px 7px rgba(0,0,0,.9), 0 0 2px rgba(0,0,0,.85);
                overflow: hidden;
                text-overflow: ellipsis;
                white-space: nowrap;
            }
            #movie-blur-sync-overlay .mbs-time {
                margin-top: 8px;
                font-size: clamp(18px, 2vw, 30px);
                line-height: 1.05;
                font-weight: 700;
                letter-spacing: .08em;
                color: rgba(210, 214, 224, .78);
                font-variant-numeric: tabular-nums;
                text-shadow: 0 2px 7px rgba(0,0,0,.9), 0 0 2px rgba(0,0,0,.85);
                white-space: nowrap;
            }
        `;
        (document.head || document.documentElement).appendChild(style);
    }

    function elementTextForClass(el) {
        try {
            return `${el.id || ''} ${el.className || ''} ${el.getAttribute && (el.getAttribute('role') || '') || ''}`.toLowerCase();
        } catch (_e) {
            return '';
        }
    }

    function isControlLikeElement(el) {
        const text = elementTextForClass(el);
        return CONTROL_LAYER_RE.test(text);
    }

    function isHiddenishElement(el) {
        try {
            if (!el || el === document.body || el === document.documentElement) return false;
            const st = window.getComputedStyle(el);
            if (!st) return false;
            if (st.display === 'none' || st.visibility === 'hidden' || Number(st.opacity) < 0.08) return true;
            return false;
        } catch (_e) {
            return false;
        }
    }

    function hostScore(el, videoRect) {
        if (!el || el === document.body || el === document.documentElement) return -999999;
        if (String(el.tagName || '').toLowerCase() === 'video') return -999999;
        if (isHiddenishElement(el)) return -500000;

        const text = elementTextForClass(el);
        const er = el.getBoundingClientRect ? el.getBoundingClientRect() : null;
        if (!er || er.width <= 30 || er.height <= 30) return -999999;

        let score = 0;

        // Слои controls/top/bottom/ui плеер часто прячет по opacity/display.
        // В них оверлей вставлять нельзя, иначе он исчезает вместе с кнопками.
        if (CONTROL_LAYER_RE.test(text)) score -= 100000;
        if (PLAYER_LAYER_RE.test(text)) score += 2000;

        if (videoRect && videoRect.width > 0 && videoRect.height > 0) {
            const dw = Math.abs(er.width - videoRect.width);
            const dh = Math.abs(er.height - videoRect.height);
            const closeW = dw <= Math.max(160, videoRect.width * 0.18);
            const closeH = dh <= Math.max(160, videoRect.height * 0.28);
            if (closeW) score += 1200;
            if (closeH) score += 1200;
            if (er.width >= videoRect.width - 3 && er.height >= videoRect.height - 3) score += 900;

            // Слишком большой внешний контейнер страницы хуже, чем корень самого плеера.
            const areaRatio = (er.width * er.height) / Math.max(1, videoRect.width * videoRect.height);
            if (areaRatio >= 0.9 && areaRatio <= 2.4) score += 900;
            if (areaRatio > 4.0) score -= 1200;
        }

        return score;
    }

    function findPlayerHost(video) {
        if (!video) return document.body;

        const fs = document.fullscreenElement || document.webkitFullscreenElement || document.mozFullScreenElement || document.msFullscreenElement;
        if (fs && fs !== video && fs.contains && fs.contains(video)) return fs;

        const videoRect = video.getBoundingClientRect ? video.getBoundingClientRect() : null;
        let best = video.parentElement || document.body;
        let bestScore = hostScore(best, videoRect);

        let el = video.parentElement;
        let guard = 0;
        while (el && el !== document.body && el !== document.documentElement && guard++ < 18) {
            const score = hostScore(el, videoRect);
            if (score > bestScore) {
                best = el;
                bestScore = score;
            }
            el = el.parentElement;
        }

        // Если fullscreen ушёл прямо на <video>, оверлей как sibling может не попасть в top layer.
        // Ниже есть перехват requestFullscreen, чтобы переводить в fullscreen стабильный host.
        if (fs && fs === video) return best || video.parentElement || document.body;

        return best || video.parentElement || document.body;
    }

    function makeHostPositioned(host) {
        if (!host || host === document.body || host === document.documentElement) return;
        try {
            const style = window.getComputedStyle(host);
            if (!style || style.position === 'static') {
                host.dataset.movieBlurSyncPositionPatched = '1';
                host.style.position = 'relative';
            }
        } catch (_e) {}
    }

    function ensureOverlay(video) {
        if (!OVERLAY_ENABLED || !video) return null;
        ensureStyle();
        if (!overlayEl) {
            overlayEl = document.createElement('div');
            overlayEl.id = 'movie-blur-sync-overlay';
            overlayEl.innerHTML = `<div class="mbs-title"></div><div class="mbs-time"></div>`;
            overlayTitleEl = overlayEl.querySelector('.mbs-title');
            overlayTimeEl = overlayEl.querySelector('.mbs-time');
        }

        const host = findPlayerHost(video);
        const useFixed = !host || host === document.body || host === document.documentElement;
        overlayEl.classList.toggle('mbs-fixed', Boolean(useFixed));
        overlayEl.classList.toggle('is-hidden', Boolean(overlayHidden));

        if (!useFixed) {
            makeHostPositioned(host);
        }

        const targetHost = useFixed ? document.body : host;
        if (targetHost && overlayEl.parentElement !== targetHost) {
            targetHost.appendChild(overlayEl);
            overlayHostEl = targetHost;
        }

        return overlayEl;
    }

    function updateOverlay(payload, video) {
        if (!OVERLAY_ENABLED || !payload || !video) return;
        const root = ensureOverlay(video);
        if (!root) return;

        const now = Date.now();
        const payloadTs = Number(payload.ts || now);
        const stale = now - payloadTs > OVERLAY_HIDE_AFTER_MS;

        overlayTitleEl.textContent = getBestTitle(payload);
        if (stale) {
            overlayTimeEl.textContent = 'Ожидание плеера…';
            return;
        }

        const current = Number(payload.currentTime || 0);
        const duration = Number(payload.duration);
        const hasDuration = Number.isFinite(duration) && duration > 0;
        overlayTimeEl.textContent = `${formatTime(current, hasDuration ? duration : current)} / ${hasDuration ? formatTime(duration, duration) : '--:--'}`;
    }

    function buildPayload(video) {
        const now = Date.now();
        return {
            currentTime: video.currentTime || 0,
            duration: Number.isFinite(video.duration) ? video.duration : null,
            paused: Boolean(video.paused),
            playbackRate: Number.isFinite(video.playbackRate) ? video.playbackRate : 1,
            ended: Boolean(video.ended),
            readyState: video.readyState,
            url: location.href,
            title: getBestTitle({ title: document.title || '' }),
            frame: window.top === window ? 'top' : 'frame',
            videoRect: rectToTopViewport(video),
            ts: now
        };
    }

    function postPayloadToTop(payload) {
        try {
            window.top.postMessage({ __movieBlurSyncVideo: true, payload }, '*');
        } catch (_e) {}
    }

    function sendCurrentTime(force = false) {
        const now = Date.now();
        if (!force && now - lastSentAt < SEND_EVERY_MS) return;
        lastSentAt = now;

        const video = pickVideo();
        if (!video) {
            log('video not found on', location.href);
            return;
        }

        const payload = buildPayload(video);
        lastPayload = payload;
        sendPayloadToApp(payload);
        postPayloadToTop(payload);
        updateOverlay(payload, video);
    }

    function installFullscreenPatch() {
        if (window.__movieBlurSyncFullscreenPatchInstalled) return;
        window.__movieBlurSyncFullscreenPatchInstalled = true;

        const patch = (proto, name) => {
            if (!proto || typeof proto[name] !== 'function') return;
            const native = proto[name];
            proto[name] = function (...args) {
                try {
                    const tag = String(this && this.tagName || '').toLowerCase();
                    if (tag === 'video') {
                        const host = findPlayerHost(this);
                        ensureOverlay(this);
                        if (host && host !== this && host !== document.body && host !== document.documentElement && typeof native === 'function') {
                            return native.apply(host, args);
                        }
                    }
                } catch (_e) {}
                return native.apply(this, args);
            };
        };

        patch(Element.prototype, 'requestFullscreen');
        patch(Element.prototype, 'webkitRequestFullscreen');
        patch(Element.prototype, 'mozRequestFullScreen');
        patch(Element.prototype, 'msRequestFullscreen');
    }

    window.addEventListener('message', function (event) {
        const data = event && event.data;
        if (data && data.__movieBlurSyncTitle === true) {
            const incomingTitle = cleanTitle(String(data.title || '').trim());
            if (isGoodTitle(incomingTitle)) {
                lastTopTitle = incomingTitle;
                forwardTitleToChildren(lastTopTitle);
                if (lastPayload && lastVideo) updateOverlay(lastPayload, lastVideo);
            }
            return;
        }

        if (data && data.__movieBlurSyncVideo === true && data.payload) {
            // В верхнем документе данные нужны только для bridge/логики названия. Оверлей рисуем в реальном video-frame.
            if (window.top === window) broadcastTitleToChildFrames(true);
        }
    });

    window.addEventListener('keydown', function (event) {
        if (event.ctrlKey && event.altKey && String(event.key || '').toLowerCase() === 'm') {
            overlayHidden = !overlayHidden;
            localStorage.setItem(STORAGE_PREFIX + 'hidden', overlayHidden ? '1' : '0');
            if (overlayEl) overlayEl.classList.toggle('is-hidden', Boolean(overlayHidden));
            event.preventDefault();
        }
    }, true);

    ['fullscreenchange', 'webkitfullscreenchange', 'mozfullscreenchange', 'MSFullscreenChange'].forEach((eventName) => {
        document.addEventListener(eventName, function () {
            if (lastPayload && lastVideo) updateOverlay(lastPayload, lastVideo);
            setTimeout(function () { if (lastPayload && lastVideo) updateOverlay(lastPayload, lastVideo); }, 250);
        }, true);
    });

    if (window.top === window) {
        broadcastTitleToChildFrames(true);
        setInterval(function () { broadcastTitleToChildFrames(false); }, 1000);
    }

    installFullscreenPatch();
    setInterval(sendCurrentTime, SEND_EVERY_MS);

    ['play', 'pause', 'seeked', 'timeupdate', 'loadedmetadata', 'loadeddata', 'canplay', 'durationchange'].forEach((eventName) => {
        window.addEventListener(eventName, function () { sendCurrentTime(true); }, true);
    });

    setTimeout(function () { broadcastTitleToChildFrames(true); sendCurrentTime(true); }, 300);
    setTimeout(function () { broadcastTitleToChildFrames(true); sendCurrentTime(true); }, 1500);
    setTimeout(function () { broadcastTitleToChildFrames(true); sendCurrentTime(true); }, 3500);
})();
