// Основная логика веб-интерфейса Humidino: связь с устройством, управление
// режимом вентиляции, зоны климата, форма настроек, панель "Устройство".
// Раздел аналитики (история запусков, график, CSV) — отдельно в stats.js.
(function () {
  "use strict";

  const qs = (id) => document.getElementById(id);

  const ZONE_ORDER = ["crawl_intake", "crawl_mid", "crawl_far", "outside"];
  const ZONE_NAME = {
    crawl_intake: "Приточка",
    crawl_mid: "Середина",
    crawl_far: "Дальний угол",
    outside: "Улица",
  };
  const ZONE_ROLE = {
    crawl_intake: "ПОДПОЛ",
    crawl_mid: "ПОДПОЛ",
    crawl_far: "ПОДПОЛ",
    outside: "УЛИЦА",
  };
  // Только приточка и улица участвуют в алгоритме осушения (relay.cpp) —
  // остальные две зоны подпола чисто информационные (см. README §4.2).
  const ZONE_IN_ALGORITHM = { crawl_intake: true, crawl_mid: false, crawl_far: false, outside: true };

  const BANNER_LABEL = {
    idle: "Ожидание",
    running: "Вентиляция работает",
    locked_condensation: "Пауза: риск конденсата",
    locked_freeze: "Пауза: защита от мороза",
    min_pause_hold: "Пауза между запусками",
    locked_sensor_fault: "Ошибка датчиков",
  };
  const BANNER_EXPLANATION = {
    idle: "Влажность в норме, вентиляция не требуется.",
    running: "Влажность выше целевого порога — идёт осушение подпола.",
    locked_condensation:
      "На улице более влажно, чем в подполе — включение временно заблокировано, чтобы не занести конденсат внутрь.",
    locked_freeze:
      "Температура подпола ниже порога защиты от замерзания — вентиляция остановлена независимо от режима.",
    min_pause_hold:
      "Соблюдается минимальная пауза между запусками, чтобы не гонять реле слишком часто.",
    locked_sensor_fault: "Не хватает исправных датчиков для безопасного автоматического решения.",
  };
  const MODE_TEXT = { auto: "АВТО", manual_on: "РУЧНОЕ ВКЛ", manual_off: "РУЧНОЕ ВЫКЛ" };
  const SEASON_TEXT = { winter: "Зима", spring: "Весна", summer: "Лето", autumn: "Осень" };

  const STATE_POLL_MS = 3000;
  const OFFLINE_AFTER_FAILURES = 2;

  let failures = 0;
  let isDemo = false;
  let runStartMs = null; // клиентское время старта текущего запуска реле (нет метки на устройстве)
  let wasRunning = false;
  let lastSettings = null; // последние настройки, полученные от устройства

  function setControlsEnabled(enabled) {
    document.querySelectorAll(".mode-switch button").forEach((btn) => {
      btn.disabled = !enabled;
    });
    qs("settingsFields").disabled = !enabled;
  }

  function fmtUptime(sec) {
    sec = Math.max(0, Math.floor(sec));
    const d = Math.floor(sec / 86400);
    const h = Math.floor((sec % 86400) / 3600);
    const m = Math.floor((sec % 3600) / 60);
    if (d > 0) return `${d} дн ${h} ч`;
    if (h > 0) return `${h} ч ${m} мин`;
    return `${m} мин`;
  }

  function fmtWifi(rssi) {
    if (rssi === undefined || rssi === null || rssi === 0) return "—";
    let quality = "Очень слабый";
    if (rssi >= -55) quality = "Отличный";
    else if (rssi >= -65) quality = "Хороший";
    else if (rssi >= -75) quality = "Слабый";
    return `${rssi} дБм · ${quality}`;
  }

  function fmtHeap(bytes) {
    if (bytes === undefined || bytes === null) return "—";
    return `${Math.round(bytes / 1024)} КБ свободно`;
  }

  function fmtRunClock(ms) {
    const totalSec = Math.max(0, Math.floor(ms / 1000));
    const m = Math.floor(totalSec / 60);
    const s = totalSec % 60;
    return `${m}:${String(s).padStart(2, "0")}`;
  }

  // --- Соединение с устройством ---

  function onFetchSuccess() {
    if (failures > 0 || qs("connectionDot").className !== "dot online") {
      setControlsEnabled(true);
    }
    failures = 0;
    qs("connectionDot").className = "dot online";
    qs("connectionText").textContent = "Подключено";
    qs("offlineNotice").hidden = true;
    qs("deviceUpdated").textContent = "Обновлено " + new Date().toLocaleTimeString("ru-RU");
  }

  function onFetchFailure() {
    failures++;
    if (failures === 1) {
      qs("connectionDot").className = "dot";
      qs("connectionText").textContent = "Переподключение…";
    } else {
      qs("connectionDot").className = "dot bad";
      qs("connectionText").textContent = "Нет связи";
      qs("offlineNotice").hidden = false;
      setControlsEnabled(false);
    }
  }

  // --- Состояние устройства (/api/state) ---

  function renderZones(zones) {
    const container = qs("zones");
    container.innerHTML = "";
    let bestHealthyRh = null;
    let intakeFault = false;
    let outsideFault = false;
    let anyFault = false;

    for (const key of ZONE_ORDER) {
      const z = zones[key];
      if (!z) continue;
      const isOutside = key === "outside";
      if (z.error) {
        anyFault = true;
        if (key === "crawl_intake") intakeFault = true;
        if (isOutside) outsideFault = true;
      } else if (!isOutside && typeof z.rh_pct === "number") {
        bestHealthyRh = bestHealthyRh === null ? z.rh_pct : Math.max(bestHealthyRh, z.rh_pct);
      }

      const article = document.createElement("article");
      article.className = "panel zone" + (z.error ? " fault" : "");

      let footText;
      if (z.error) {
        footText = "Нет свежих данных";
      } else if (!isOutside && typeof z.dew_c === "number") {
        footText = `Точка росы ${z.dew_c.toFixed(1)}°C · ${ZONE_IN_ALGORITHM[key] ? "участвует в алгоритме" : "информационный датчик"}`;
      } else {
        footText = ZONE_IN_ALGORITHM[key] ? "Участвует в алгоритме" : "Информационный датчик";
      }

      const rhText = z.error || typeof z.rh_pct !== "number" ? "—" : z.rh_pct.toFixed(1);
      const tempText = z.error || typeof z.temp_c !== "number" ? "—" : z.temp_c.toFixed(1) + "°C";

      article.innerHTML =
        `<div class="zone-head"><span>${ZONE_ROLE[key]}</span><span class="dot ${z.error ? "bad" : "online"}"></span></div>` +
        `<h3>${ZONE_NAME[key] || key}</h3>` +
        `<div class="zone-rh">${rhText}<small>%</small></div>` +
        `<div class="zone-temp">${tempText}</div>` +
        `<div class="zone-foot">${footText}</div>`;
      container.appendChild(article);
    }

    qs("crawlHumidity").textContent = bestHealthyRh === null ? "—" : bestHealthyRh.toFixed(1);
    qs("humidityFill").style.width = (bestHealthyRh === null ? 0 : Math.min(100, Math.max(0, bestHealthyRh))) + "%";

    const dot = qs("sensorDot");
    const health = qs("sensorHealth");
    if (intakeFault || outsideFault) {
      dot.className = "dot bad";
      const failed = [intakeFault && "приточка", outsideFault && "улица"].filter(Boolean).join(", ");
      health.textContent = `Ошибка датчика (${failed}) — влияет на автоматику`;
    } else if (anyFault) {
      dot.className = "dot";
      health.textContent = "Информационный датчик подпола неисправен, на автоматику не влияет";
    } else {
      dot.className = "dot online";
      health.textContent = "Все датчики в норме";
    }
  }

  function renderRunTimer(relayOn, cycleCount) {
    if (relayOn) {
      if (runStartMs === null) runStartMs = Date.now();
      qs("runTimer").textContent = "Работает " + fmtRunClock(Date.now() - runStartMs);
    } else {
      runStartMs = null;
      qs("runTimer").textContent = cycleCount !== undefined ? `Циклов всего: ${cycleCount}` : "—";
    }
  }

  function renderState(s) {
    isDemo = !!s.demo;
    qs("demoNotice").hidden = !isDemo;

    qs("seasonBadge").textContent = "Сезон: " + (SEASON_TEXT[s.season] || s.season || "—");

    const stateKey = (s.relay && s.relay.state_str) || "idle";
    const relayOn = !!(s.relay && s.relay.on);
    qs("relayActual").textContent = relayOn ? "РЕЛЕ: ВКЛЮЧЕНО" : "РЕЛЕ: ВЫКЛЮЧЕНО";
    qs("bannerLabel").textContent = BANNER_LABEL[stateKey] || stateKey;
    qs("statusExplanation").textContent = BANNER_EXPLANATION[stateKey] || "";
    renderRunTimer(relayOn, s.relay && s.relay.cycle_count);

    const fan = qs("fanIcon");
    fan.classList.toggle("spinning", relayOn);
    wasRunning = relayOn;

    if (s.zones) renderZones(s.zones);

    qs("deviceUptime").textContent = s.uptime_s !== undefined ? fmtUptime(s.uptime_s) : "—";
    qs("deviceWifi").textContent = fmtWifi(s.wifi_rssi);
    qs("deviceMemory").textContent = fmtHeap(s.free_heap);
  }

  async function refreshState() {
    try {
      const r = await fetch("/api/state");
      if (!r.ok) throw new Error("HTTP " + r.status);
      const s = await r.json();
      onFetchSuccess();
      renderState(s);
    } catch (e) {
      onFetchFailure();
    }
  }

  // Живой тикер таймера запуска — обновляет только текст, без опроса сети,
  // чтобы счётчик шёл плавно между опросами /api/state раз в 3 секунды.
  setInterval(() => {
    if (wasRunning && runStartMs !== null) {
      qs("runTimer").textContent = "Работает " + fmtRunClock(Date.now() - runStartMs);
    }
  }, 1000);

  // --- Режим вентиляции ---

  function setModeButtons(mode) {
    qs("modeBadge").textContent = MODE_TEXT[mode] || mode;
    document.querySelectorAll(".mode-switch button").forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.mode === mode);
    });
  }

  async function setMode(mode) {
    const previous = lastSettings ? lastSettings.mode : null;
    setModeButtons(mode); // мгновенный отклик, не дожидаясь ответа сервера
    try {
      const r = await fetch("/api/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
      if (!r.ok) throw new Error("HTTP " + r.status);
      applySettings(await r.json());
    } catch (e) {
      if (previous) setModeButtons(previous);
      showToast("Не удалось сменить режим", true);
    }
  }

  document.querySelectorAll(".mode-switch button").forEach((btn) => {
    btn.addEventListener("click", () => setMode(btn.dataset.mode));
  });

  // --- Настройки ---

  const FIELD_IDS = ["rh_target", "hysteresis_pct", "freeze_c", "min_runtime_min", "min_pause_min"];

  function msToMin(ms) {
    return Math.round(ms / 60000);
  }

  function fillSettingsForm(s) {
    qs("rh_target").value = s.rh_target;
    qs("hysteresis_pct").value = s.hysteresis_pct;
    qs("freeze_c").value = s.freeze_c;
    qs("min_runtime_min").value = msToMin(s.min_runtime_ms);
    qs("min_pause_min").value = msToMin(s.min_pause_ms);
    qs("season_auto").checked = !!s.season_auto;
    updateDirtyBadge();
    updateRulePreview();
  }

  function applySettings(s) {
    lastSettings = s;
    setModeButtons(s.mode || "auto");
    fillSettingsForm(s);
  }

  function currentFormValues() {
    return {
      rh_target: parseFloat(qs("rh_target").value),
      hysteresis_pct: parseFloat(qs("hysteresis_pct").value),
      freeze_c: parseFloat(qs("freeze_c").value),
      min_runtime_ms: Math.round(parseFloat(qs("min_runtime_min").value || "0") * 60000),
      min_pause_ms: Math.round(parseFloat(qs("min_pause_min").value || "0") * 60000),
    };
  }

  function updateDirtyBadge() {
    if (!lastSettings) return;
    const v = currentFormValues();
    const dirty =
      v.rh_target !== lastSettings.rh_target ||
      v.hysteresis_pct !== lastSettings.hysteresis_pct ||
      v.freeze_c !== lastSettings.freeze_c ||
      v.min_runtime_ms !== lastSettings.min_runtime_ms ||
      v.min_pause_ms !== lastSettings.min_pause_ms;
    qs("dirtyBadge").hidden = !dirty;
  }

  function updateRulePreview() {
    const rhTarget = parseFloat(qs("rh_target").value);
    const hysteresis = parseFloat(qs("hysteresis_pct").value);
    const minRuntime = qs("min_runtime_min").value;
    const minPause = qs("min_pause_min").value;
    if ([rhTarget, hysteresis].some(Number.isNaN)) {
      qs("rulePreview").textContent = "Заполните поля, чтобы увидеть итоговое правило.";
      return;
    }
    let text =
      `Вентиляция включится, если влажность в подполе поднимется выше ${rhTarget}%, и будет работать, ` +
      `пока показания не опустятся до ${(rhTarget - hysteresis).toFixed(1)}% (гистерезис ${hysteresis} п.п.). ` +
      `Минимум работы — ${minRuntime || 0} мин, пауза между запусками — не меньше ${minPause || 0} мин.`;
    if (hysteresis >= rhTarget) {
      text += " Внимание: гистерезис не меньше целевой влажности — вентиляция не сможет остановиться по графику.";
    }
    qs("rulePreview").textContent = text;

    const marker = qs("targetMarker");
    marker.style.left = Math.min(100, Math.max(0, rhTarget)) + "%";
    qs("targetLabel").textContent = `Порог ${rhTarget}%`;
  }

  FIELD_IDS.forEach((id) => {
    qs(id).addEventListener("input", () => {
      updateDirtyBadge();
      updateRulePreview();
    });
  });

  async function loadSettings() {
    try {
      const r = await fetch("/api/settings");
      if (!r.ok) throw new Error("HTTP " + r.status);
      applySettings(await r.json());
      setControlsEnabled(true);
    } catch (e) {
      // повторится: соединение и так помечается офлайн через refreshState
    }
  }

  qs("settingsForm").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const body = currentFormValues();
    const status = qs("saveStatus");
    status.textContent = "Сохраняем…";
    try {
      const r = await fetch("/api/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!r.ok) throw new Error("HTTP " + r.status);
      applySettings(await r.json());
      status.textContent = "Сохранено " + new Date().toLocaleTimeString("ru-RU");
      showToast("Настройки сохранены");
    } catch (e) {
      status.textContent = "Ошибка сохранения";
      showToast("Не удалось сохранить настройки", true);
    }
  });

  qs("resetSettings").addEventListener("click", () => {
    if (lastSettings) fillSettingsForm(lastSettings);
    qs("saveStatus").textContent = "";
  });

  qs("season_auto").addEventListener("change", async (ev) => {
    const checked = ev.target.checked;
    ev.target.disabled = true;
    try {
      const r = await fetch("/api/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ season_auto: checked }),
      });
      if (!r.ok) throw new Error("HTTP " + r.status);
      applySettings(await r.json());
    } catch (e) {
      ev.target.checked = !checked;
      showToast("Не удалось сохранить настройки", true);
    } finally {
      ev.target.disabled = false;
    }
  });

  // --- Тост-уведомления ---

  let toastTimer = null;
  function showToast(message, isError) {
    const el = qs("toast");
    el.textContent = message;
    el.className = "toast" + (isError ? " error" : "");
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      el.hidden = true;
    }, 3000);
  }

  // --- Навигация по разделам ---

  const VIEWS = ["overview", "statistics", "settings"];

  function showView(name) {
    if (!VIEWS.includes(name)) name = "overview";
    for (const v of VIEWS) {
      qs(v).hidden = v !== name;
    }
    document.querySelectorAll(".sidebar nav a[data-view]").forEach((a) => {
      a.classList.toggle("selected", a.dataset.view === name);
    });
  }

  window.addEventListener("hashchange", () => showView(location.hash.slice(1)));
  showView(location.hash.slice(1));

  // --- Запуск ---

  refreshState();
  loadSettings();
  setInterval(refreshState, STATE_POLL_MS);
})();
