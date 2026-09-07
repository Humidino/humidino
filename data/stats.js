// Раздел "Аналитика и статистика" (данные из run_log.h через /api/history* —
// см. README §4.2): сводка "Сегодня" на обзорной странице, столбчатый график,
// таблица журнала запусков с фильтром, выгрузка в CSV.
(function () {
  "use strict";

  const qs = (id) => document.getElementById(id);

  const PAGE_SIZE = 200; // потолок одного запроса в web_server.cpp
  const ROWS_PAGE = 20; // сколько строк журнала показывать за раз ("Показать ещё")
  const MAX_ALL_TIME_CHART_DAYS = 60; // не растягиваем график на весь журнал целиком

  const STOP_REASON_TEXT = {
    hysteresis_reached: "По графику (влажность в норме)",
    manual_off: "Выключено вручную",
    locked_freeze: "Прервано: защита от замерзания",
    locked_condensation: "Прервано: защита от конденсата",
    sensor_fault: "Прервано: ошибка датчика",
    interrupted: "Прервано перезагрузкой устройства",
    unknown: "Работает сейчас",
  };

  const REASON_CATEGORY = {
    hysteresis_reached: "normal",
    manual_off: "manual",
    locked_freeze: "protection",
    locked_condensation: "protection",
    sensor_fault: "protection",
    interrupted: "interrupted",
  };

  let deviceTzOffsetSec = 0;
  let timeSynced = false;
  let periodDays = 7; // 1 / 7 / 14 / 30 / 0 (весь журнал)
  let allRecords = []; // записи, загруженные для текущего периода (от новой к старой)
  let visibleRows = ROWS_PAGE;

  function fmtDurationSec(sec) {
    if (!sec && sec !== 0) return "—";
    const m = Math.round(sec / 60);
    if (m < 60) return `${m} мин`;
    return `${Math.floor(m / 60)} ч ${m % 60} мин`;
  }

  function fmtEpochLocal(epoch) {
    if (!epoch) return "—";
    return new Date(epoch * 1000).toLocaleString("ru-RU", {
      day: "2-digit",
      month: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
    });
  }

  function fmtValue(v, digits) {
    digits = digits === undefined ? 1 : digits;
    return v === null || v === undefined || Number.isNaN(v) ? "—" : v.toFixed(digits);
  }

  function nowSec() {
    return Date.now() / 1000;
  }

  function deviceDayNumber(epochSec) {
    return Math.floor((epochSec + deviceTzOffsetSec) / 86400);
  }

  function dayStartEpoch(dayNumber) {
    return dayNumber * 86400 - deviceTzOffsetSec;
  }

  // --- Сводка (сегодня / всего) — используется и на обзорной странице ---

  async function loadHistorySummary() {
    const r = await fetch("/api/history/summary");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const s = await r.json();
    deviceTzOffsetSec = Number(s.local_tz_offset_sec) || 0;
    timeSynced = !!s.time_synced;

    qs("todayRuns").textContent = timeSynced ? s.runs_today : "—";
    qs("todayRuntime").textContent = timeSynced ? fmtDurationSec(s.runtime_today_s) : "—";
    qs("totalRuns").textContent = s.runs_total;
    qs("logCount").textContent = `${s.log_count} из ${s.log_capacity} записей журнала`;
    return s;
  }

  // --- Загрузка записей за период ---

  async function fetchRecords(minEpochOrNull) {
    const records = [];
    let offset = 0;
    for (;;) {
      const r = await fetch(`/api/history?limit=${PAGE_SIZE}&offset=${offset}`);
      if (!r.ok) throw new Error("HTTP " + r.status);
      const page = await r.json();
      records.push(...page);
      const reachedWindow =
        minEpochOrNull !== null && page.some((rec) => rec.start_epoch && rec.start_epoch < minEpochOrNull);
      if (page.length < PAGE_SIZE || reachedWindow || offset > 2000) break;
      offset += page.length;
    }
    return records;
  }

  function periodWindowStart() {
    if (periodDays === 0) return null; // весь журнал — без нижней границы
    return dayStartEpoch(deviceDayNumber(nowSec()) - (periodDays - 1));
  }

  // --- Сводные метрики периода ---

  function overlapSeconds(rec, from, to) {
    const start = rec.start_epoch || 0;
    if (!start) return 0;
    const end = rec.in_progress ? nowSec() : rec.end_epoch || start;
    return Math.max(0, Math.min(end, to) - Math.max(start, from));
  }

  function renderPeriodSummary(records, windowStart) {
    const to = nowSec();
    const from = windowStart === null ? 0 : windowStart;
    const inWindow = records.filter((r) => r.start_epoch && r.start_epoch >= from);

    qs("periodRuns").textContent = inWindow.length;
    const runtimeSec = records.reduce((sum, r) => sum + overlapSeconds(r, from, to), 0);
    qs("periodRuntime").textContent = fmtDurationSec(runtimeSec);

    const deltas = inWindow
      .filter((r) => !r.in_progress && typeof r.start.crawl_rh === "number" && typeof r.end.crawl_rh === "number")
      .map((r) => r.end.crawl_rh - r.start.crawl_rh);
    if (deltas.length === 0) {
      qs("periodDelta").textContent = "—";
    } else {
      const avg = deltas.reduce((a, b) => a + b, 0) / deltas.length;
      qs("periodDelta").textContent = `${avg >= 0 ? "+" : ""}${avg.toFixed(1)} п.п.`;
    }

    const labels = { 1: "Сегодня", 7: "Последние 7 дней", 14: "Последние 14 дней", 30: "Последние 30 дней", 0: "Весь журнал" };
    qs("periodLabel").textContent = labels[periodDays] || "";
  }

  // --- График ---

  function computeBuckets() {
    const buckets = [];
    if (periodDays === 1) {
      const dayStart = dayStartEpoch(deviceDayNumber(nowSec()));
      for (let h = 0; h < 24; h++) {
        buckets.push({ from: dayStart + h * 3600, to: dayStart + (h + 1) * 3600, label: h % 4 === 0 ? String(h).padStart(2, "0") : "" });
      }
      return buckets;
    }

    let spanDays = periodDays;
    if (periodDays === 0) {
      const earliest = allRecords.reduce((min, r) => (r.start_epoch && r.start_epoch < min ? r.start_epoch : min), nowSec());
      spanDays = Math.min(MAX_ALL_TIME_CHART_DAYS, Math.max(1, deviceDayNumber(nowSec()) - deviceDayNumber(earliest) + 1));
    }
    const today = deviceDayNumber(nowSec());
    const labelEvery = Math.max(1, Math.ceil(spanDays / 8));
    for (let i = spanDays - 1; i >= 0; i--) {
      const dayNumber = today - i;
      const from = dayStartEpoch(dayNumber);
      const label = i % labelEvery === 0 ? new Date(from * 1000).toISOString().slice(5, 10) : "";
      buckets.push({ from, to: from + 86400, label });
    }
    return buckets;
  }

  function renderChart(records) {
    const buckets = computeBuckets();
    const counts = buckets.map((b) => records.filter((r) => r.start_epoch && r.start_epoch >= b.from && r.start_epoch < b.to).length);
    const max = Math.max(1, ...counts);

    const width = 640;
    const height = 220;
    const barGap = Math.max(1, Math.floor(width / buckets.length / 8));
    const barW = width / buckets.length - barGap;

    const svgNS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(svgNS, "svg");
    svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    svg.setAttribute("width", "100%");
    svg.setAttribute("height", height);

    buckets.forEach((bucket, i) => {
      const c = counts[i];
      const barH = (c / max) * (height - 28);
      const x = i * (barW + barGap);
      const rect = document.createElementNS(svgNS, "rect");
      rect.setAttribute("x", x);
      rect.setAttribute("y", height - barH - 20);
      rect.setAttribute("width", Math.max(1, barW));
      rect.setAttribute("height", barH);
      rect.setAttribute("rx", 2);
      rect.setAttribute("fill", c > 0 ? "var(--accent)" : "var(--line)");
      svg.appendChild(rect);

      if (c > 0) {
        const countText = document.createElementNS(svgNS, "text");
        countText.setAttribute("x", x + barW / 2);
        countText.setAttribute("y", height - barH - 24);
        countText.setAttribute("font-size", "10");
        countText.setAttribute("fill", "var(--muted)");
        countText.setAttribute("text-anchor", "middle");
        countText.textContent = c;
        svg.appendChild(countText);
      }
      if (bucket.label) {
        const labelText = document.createElementNS(svgNS, "text");
        labelText.setAttribute("x", x + barW / 2);
        labelText.setAttribute("y", height - 4);
        labelText.setAttribute("font-size", "10");
        labelText.setAttribute("fill", "var(--muted)");
        labelText.setAttribute("text-anchor", "middle");
        labelText.textContent = bucket.label;
        svg.appendChild(labelText);
      }
    });

    const container = qs("historyChart");
    container.innerHTML = "";
    container.appendChild(svg);

    qs("chartNote").textContent =
      periodDays === 1
        ? "Запусков по часам (время устройства)."
        : periodDays === 0 && buckets.length >= MAX_ALL_TIME_CHART_DAYS
          ? `По сохранённым запускам. Показаны последние ${MAX_ALL_TIME_CHART_DAYS} дней — более старые видны в таблице ниже.`
          : "По сохранённым запускам. Запуск через полночь разделяется между днями.";
  }

  // --- Таблица журнала ---

  function categoryFor(rec) {
    if (rec.in_progress) return "active";
    return REASON_CATEGORY[rec.stop_reason] || "normal";
  }

  function filteredRecords() {
    const filter = qs("reasonFilter").value;
    if (filter === "all") return allRecords;
    return allRecords.filter((r) => categoryFor(r) === filter);
  }

  function renderHistoryRows() {
    const records = filteredRecords();
    const rowsEl = qs("historyRows");
    const emptyEl = qs("historyEmpty");
    rowsEl.innerHTML = "";

    if (records.length === 0) {
      emptyEl.hidden = false;
      qs("showMore").hidden = true;
      return;
    }
    emptyEl.hidden = true;

    const visible = records.slice(0, visibleRows);
    for (const rec of visible) {
      const row = document.createElement("details");
      row.className = "history-row";
      const reasonLabel = rec.in_progress ? "Работает сейчас" : STOP_REASON_TEXT[rec.stop_reason] || rec.stop_reason;
      const durLabel = rec.in_progress ? "идёт…" : fmtDurationSec(rec.duration_s);
      row.innerHTML =
        `<summary><span>${fmtEpochLocal(rec.start_epoch)}</span><span class="history-dur">${durLabel}</span><span class="history-reason">${reasonLabel}</span></summary>` +
        `<div class="history-zones">` +
        `<span>Подпол: ${fmtValue(rec.start.crawl_rh)}→${fmtValue(rec.end.crawl_rh)}% · ${fmtValue(rec.start.crawl_temp_c)}→${fmtValue(rec.end.crawl_temp_c)}°C</span>` +
        `<span>Улица: ${fmtValue(rec.start.outside_rh)}→${fmtValue(rec.end.outside_rh)}% · ${fmtValue(rec.start.outside_temp_c)}→${fmtValue(rec.end.outside_temp_c)}°C</span>` +
        `</div>`;
      rowsEl.appendChild(row);
    }

    qs("showMore").hidden = records.length <= visible.length;
  }

  qs("showMore").addEventListener("click", () => {
    visibleRows += ROWS_PAGE;
    renderHistoryRows();
  });

  qs("reasonFilter").addEventListener("change", () => {
    visibleRows = ROWS_PAGE;
    renderHistoryRows();
  });

  // --- CSV ---

  function toCsv(records) {
    const header = [
      "Начало",
      "Окончание",
      "Длительность, с",
      "Причина остановки",
      "Подпол начало, %",
      "Подпол начало, °C",
      "Подпол конец, %",
      "Подпол конец, °C",
      "Улица начало, %",
      "Улица начало, °C",
      "Улица конец, %",
      "Улица конец, °C",
    ];
    const rows = records.map((r) => [
      r.start_epoch ? new Date(r.start_epoch * 1000).toISOString() : "",
      r.end_epoch ? new Date(r.end_epoch * 1000).toISOString() : "",
      r.duration_s ?? "",
      STOP_REASON_TEXT[r.stop_reason] || r.stop_reason,
      r.start.crawl_rh ?? "",
      r.start.crawl_temp_c ?? "",
      r.end.crawl_rh ?? "",
      r.end.crawl_temp_c ?? "",
      r.start.outside_rh ?? "",
      r.start.outside_temp_c ?? "",
      r.end.outside_rh ?? "",
      r.end.outside_temp_c ?? "",
    ]);
    return [header, ...rows].map((row) => row.join(",")).join("\r\n");
  }

  qs("exportCsv").addEventListener("click", () => {
    const csv = "﻿" + toCsv(filteredRecords());
    const blob = new Blob([csv], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    const stamp = new Date().toISOString().slice(0, 10);
    a.href = url;
    a.download = `humidino-history-${stamp}.csv`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  });

  // --- Оркестрация загрузки ---

  async function loadHistory() {
    const notice = qs("historyNotice");
    notice.hidden = true;
    try {
      await loadHistorySummary();
      allRecords = await fetchRecords(periodWindowStart());
      visibleRows = ROWS_PAGE;

      if (!timeSynced) {
        notice.hidden = false;
        notice.textContent =
          "Время устройства не синхронизировано — группировка по дням недоступна, показаны все сохранённые записи.";
      }

      renderPeriodSummary(allRecords, periodWindowStart());
      renderChart(allRecords);
      renderHistoryRows();
      qs("exportCsv").disabled = allRecords.length === 0;
    } catch (e) {
      notice.hidden = false;
      notice.textContent = "Не удалось загрузить историю запусков.";
    }
  }

  document.querySelectorAll(".periods button[data-days]").forEach((btn) => {
    btn.addEventListener("click", () => {
      periodDays = parseInt(btn.dataset.days, 10);
      document.querySelectorAll(".periods button").forEach((b) => {
        b.classList.toggle("active", b === btn);
        b.setAttribute("aria-pressed", b === btn ? "true" : "false");
      });
      loadHistory();
    });
  });

  qs("refreshHistory").addEventListener("click", loadHistory);

  loadHistory();
  setInterval(loadHistorySummary, 30000);
  // Журнал меняется редко (минимум раз в MIN_RUNTIME_MS/MIN_PAUSE_MS) —
  // незачем опрашивать чаще.
  setInterval(loadHistory, 30000);
})();
