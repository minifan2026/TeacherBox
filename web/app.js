// TeacherBox 前端逻辑：通过 WebMessage(JSON) 与 gui.exe 通信
(function () {
  "use strict";

  var bridge = window.chrome && window.chrome.webview ? window.chrome.webview : null;

  var state = {
    tools: [],
    status: {},   // id -> 'unknown' | 'ok' | 'miss'
    busy: {},     // id -> 'check' | 'install'
    filter: "",
    toolsDir: "",
    view: "local",
    market: [],
    marketNote: "",
    marketBusy: {},
    marketLoaded: false,
    marketOffset: 0,
    marketTotal: 0,
    marketLoading: false,
    pending: [],
    pendingDir: ""
  };

  var elGrid = document.getElementById("grid");
  var elEmpty = document.getElementById("empty");
  var elWarn = document.getElementById("warnings");
  var elLog = document.getElementById("log");
  var elCounter = document.getElementById("counter");
  var elConsole = document.querySelector(".console");
  var elVersion = document.getElementById("version");
  var elSearch = document.getElementById("search");
  var elModal = document.getElementById("modal");
  var elModalTitle = document.getElementById("modal-title");
  var elModalSub = document.getElementById("modal-sub");
  var elModalBody = document.getElementById("modal-body");
  var elViewLocal = document.getElementById("view-local");
  var elViewMarket = document.getElementById("view-market");
  var elMarketGrid = document.getElementById("market-grid");
  var elMarketEmpty = document.getElementById("market-empty");
  var elMarketError = document.getElementById("market-error");
  var elMarketNote = document.getElementById("market-note");
  var elContent = document.querySelector(".content");

  function send(payload) {
    if (bridge) bridge.postMessage(payload);
  }

  // ------------------------------------------------------------ 日志
  function log(text, kind) {
    var stamp = new Date().toLocaleTimeString("zh-CN", { hour12: false });
    var prefix = "[" + stamp + "] ";
    var line = prefix + text + "\n";
    if (elConsole) elConsole.classList.remove("hidden");
    elLog.textContent += line;
    if (elLog.textContent.length > 120000) {
      elLog.textContent = elLog.textContent.slice(-90000);
    }
    elLog.scrollTop = elLog.scrollHeight;
  }

  // ------------------------------------------------------------ Markdown
  function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function inline(s) {
    return s
      .replace(/`([^`]+)`/g, "<code>$1</code>")
      .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
      .replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2" target="_blank">$1</a>');
  }

  function markdownToHtml(md) {
    var blocks = md.replace(/\r\n/g, "\n").split(/```/);
    var html = "";
    for (var i = 0; i < blocks.length; i++) {
      var block = blocks[i];
      if (i % 2 === 1) {
        var firstLine = block.indexOf("\n");
        var lang = firstLine >= 0 ? block.slice(0, firstLine).trim() : "";
        var code = firstLine >= 0 ? block.slice(firstLine + 1) : block;
        html += "<pre><code>" + escapeHtml(code.replace(/\n$/, "")) + "</code></pre>";
        continue;
      }
      var lines = block.split("\n");
      var listType = null;
      var para = [];
      function flushPara() {
        if (para.length) {
          html += "<p>" + inline(escapeHtml(para.join(" "))) + "</p>";
          para = [];
        }
      }
      function closeList() {
        if (listType) {
          html += "</" + listType + ">";
          listType = null;
        }
      }
      lines.forEach(function (raw) {
        var line = raw.replace(/\s+$/, "");
        var m;
        if (!line.trim()) { flushPara(); closeList(); return; }
        if ((m = line.match(/^(#{1,4})\s+(.*)$/))) {
          flushPara(); closeList();
          var lvl = Math.min(m[1].length, 4);
          html += "<h" + lvl + ">" + inline(escapeHtml(m[2])) + "</h" + lvl + ">";
          return;
        }
        if (/^>\s?/.test(line)) {
          flushPara(); closeList();
          html += "<blockquote>" + inline(escapeHtml(line.replace(/^>\s?/, ""))) + "</blockquote>";
          return;
        }
        if (/^\s*[-*+]\s+/.test(line)) {
          flushPara();
          if (listType !== "ul") { closeList(); html += "<ul>"; listType = "ul"; }
          html += "<li>" + inline(escapeHtml(line.replace(/^\s*[-*+]\s+/, ""))) + "</li>";
          return;
        }
        if ((m = line.match(/^\s*(\d+)\.\s+(.*)$/))) {
          flushPara();
          if (listType !== "ol") { closeList(); html += "<ol>"; listType = "ol"; }
          html += "<li>" + inline(escapeHtml(m[2])) + "</li>";
          return;
        }
        if (/^---+$/.test(line.trim())) {
          flushPara(); closeList(); html += "<hr>"; return;
        }
        closeList();
        para.push(line.trim());
      });
      flushPara();
      closeList();
    }
    return html;
  }

  // ------------------------------------------------------------ 渲染
  function statusChip(id) {
    var busy = state.busy[id];
    if (busy === "check") return { cls: "busy", text: "检测中…" };
    if (busy === "install") return { cls: "busy", text: "安装中…" };
    var st = state.status[id];
    if (st === "ok") return { cls: "ok", text: "环境就绪" };
    if (st === "miss") return { cls: "miss", text: "缺少环境" };
    return { cls: "", text: "未检测" };
  }

  function makeButton(label, cls, onClick) {
    var b = document.createElement("button");
    b.className = "btn " + (cls || "");
    b.textContent = label;
    b.addEventListener("click", onClick);
    return b;
  }

  function renderCard(tool) {
    var card = document.createElement("div");
    card.className = "card";

    // 左：编号（台账式，等宽体）
    var idEl = document.createElement("div");
    idEl.className = "card-id";
    idEl.textContent = tool.id;
    idEl.title = tool.id;
    card.appendChild(idEl);

    // 中：标题行（名称 + 版本 + 状态）/ 描述
    var main = document.createElement("div");
    main.className = "card-main";

    var line = document.createElement("div");
    line.className = "card-line";

    // 状态点放在名称前，纵向对齐成一条列
    var chip = document.createElement("span");
    var c = statusChip(tool.id);
    chip.className = "chip " + c.cls;
    chip.title = c.text;
    line.appendChild(chip);

    var title = document.createElement("h3");
    title.className = "card-title";
    title.textContent = tool.name || tool.id;
    line.appendChild(title);

    if (tool.version) {
      var ver = document.createElement("span");
      ver.className = "card-ver";
      ver.textContent = "v" + tool.version;
      line.appendChild(ver);
    }

    main.appendChild(line);

    if (tool.description) {
      var desc = document.createElement("p");
      desc.className = "card-desc";
      desc.textContent = tool.description;
      main.appendChild(desc);
    }

    card.appendChild(main);

    // 右：操作
    var side = document.createElement("div");
    side.className = "card-side";

    var actions = document.createElement("div");
    actions.className = "card-actions";

    var busy = !!state.busy[tool.id];

    if (tool.hasCheck) {
      var bCheck = makeButton("检测", "ghost", function () {
        log("检测 " + tool.id);
        send({ cmd: "check", id: tool.id });
      });
      bCheck.disabled = busy;
      actions.appendChild(bCheck);
    }
    if (tool.hasInstall) {
      var bInstall = makeButton("安装", "primary", function () {
        log("安装 " + tool.id);
        send({ cmd: "install", id: tool.id });
      });
      bInstall.disabled = busy;
      actions.appendChild(bInstall);
    }
    if (tool.hasHelp) {
      actions.appendChild(makeButton("帮助", "ghost", function () {
        send({ cmd: "help", id: tool.id });
      }));
    }
    (tool.actions || []).forEach(function (label, index) {
      actions.appendChild(makeButton(label, "ghost", function () {
        send({ cmd: "action", id: tool.id, index: index });
      }));
    });

    side.appendChild(actions);
    card.appendChild(side);
    return card;
  }

  function pad2(n) {
    return n < 10 ? "0" + n : String(n);
  }

  function render() {
    var q = state.filter.trim().toLowerCase();
    var visible = state.tools.filter(function (t) {
      if (!q) return true;
      return (t.id + " " + t.name + " " + (t.description || "")).toLowerCase().indexOf(q) >= 0;
    });

    elGrid.textContent = "";
    visible.forEach(function (t) { elGrid.appendChild(renderCard(t)); });
    elCounter.textContent = String(visible.length);
    elEmpty.classList.toggle("hidden", state.tools.length > 0);
  }

  function showWarnings(list) {
    if (!list || !list.length) {
      elWarn.classList.add("hidden");
      elWarn.textContent = "";
      return;
    }
    elWarn.classList.remove("hidden");
    elWarn.textContent = "已忽略 " + list.length + " 项：\n" + list.join("\n");
  }

  // ------------------------------------------------------------ 工具市场
  function renderMarketCard(item) {
    var card = document.createElement("div");
    card.className = "card";

    var local = item.localVersion || "";
    var hasUpdate = !!item.installed && !!item.version && local !== item.version;

    var idEl = document.createElement("div");
    idEl.className = "card-id";
    idEl.textContent = item.id;
    idEl.title = item.id;
    card.appendChild(idEl);

    var main = document.createElement("div");
    main.className = "card-main";

    var line = document.createElement("div");
    line.className = "card-line";
    var dot = document.createElement("span");
    dot.className = "chip " + (item.installed ? (hasUpdate ? "upd" : "ok") : "");
    line.appendChild(dot);
    var title = document.createElement("h3");
    title.className = "card-title";
    title.textContent = item.name || item.id;
    line.appendChild(title);
    if (item.dev) {
      var tag = document.createElement("span");
      tag.className = "tag";
      tag.textContent = "开发者";
      line.appendChild(tag);
    }
    if (item.version) {
      var ver = document.createElement("span");
      ver.className = "card-ver";
      ver.textContent = "v" + item.version;
      line.appendChild(ver);
    }
    if (item.installed) {
      var localTag = document.createElement("span");
      localTag.className = "card-ver card-local";
      localTag.textContent = local ? "已装 v" + local : "已装（版本未知）";
      line.appendChild(localTag);
    }
    if (hasUpdate) {
      var updTag = document.createElement("span");
      updTag.className = "tag upd";
      updTag.textContent = "有新版本";
      line.appendChild(updTag);
    }
    main.appendChild(line);

    if (item.description) {
      var desc = document.createElement("p");
      desc.className = "card-desc";
      desc.textContent = item.description;
      main.appendChild(desc);
    }
    card.appendChild(main);

    var side = document.createElement("div");
    side.className = "card-side";
    var acts = document.createElement("div");
    acts.className = "card-actions";

    var busy = !!state.marketBusy[item.id];
    if (item.installed && !hasUpdate) {
      var b = makeButton("装好了", "ghost", function () {});
      b.disabled = true;
      acts.appendChild(b);
    } else {
      var label = hasUpdate ? (busy ? "更新中…" : "更新") : (busy ? "下载中…" : "下载");
      var dl = makeButton(label, "primary", function () {
        log(label.replace("…", "") + " " + item.id);
        state.marketBusy[item.id] = true;
        renderMarket();
        send({ cmd: "marketInstall", id: item.id });
      });
      dl.disabled = busy;
      acts.appendChild(dl);
    }
    side.appendChild(acts);
    card.appendChild(side);
    return card;
  }

  // ------------------------------------------------------------ 市场分页
  // 滚动到底就拉下一页（用户翻到第几页就拉到第几页），也保留「加载更多」按钮兜底
  function marketCanMore() {
    return state.view === "market" && state.marketLoaded && !state.marketLoading &&
           state.marketTotal > 0 && state.market.length < state.marketTotal;
  }

  function requestMoreMarket() {
    if (!marketCanMore()) return;
    state.marketLoading = true;
    renderMarket();
    send({ cmd: "marketMore" });
  }

  var morePending = false;
  function marketFillCheck() {
    if (morePending || !marketCanMore()) return;
    if (!elContent) return;
    // 这一页还没填满一屏（或者滚动条快到低了），继续拉
    if (elContent.scrollHeight <= elContent.clientHeight + 60 ||
        elContent.scrollTop + elContent.clientHeight >= elContent.scrollHeight - 220) {
      morePending = true;
      requestMoreMarket();
      setTimeout(function () { morePending = false; }, 600);
    }
  }

  function renderMarket() {
    elMarketGrid.textContent = "";
    state.market.forEach(function (m) { elMarketGrid.appendChild(renderMarketCard(m)); });
    if (state.market.length === 0) {
      elMarketEmpty.classList.remove("hidden");
      elMarketEmpty.textContent = state.marketLoading ? "正在拉清单…" : "清单是空的。";
    } else {
      elMarketEmpty.classList.add("hidden");
    }
    elCounter.textContent = state.view === "market"
      ? String(state.market.length)
      : String(state.tools.length);

    var more = document.getElementById("btn-market-more");
    var stat = document.getElementById("market-stat");
    if (state.marketTotal > 0) {
      stat.textContent = "已显示 " + state.market.length + " / 共 " + state.marketTotal + " 个";
      var canMore = state.market.length < state.marketTotal;
      more.classList.toggle("hidden", !canMore);
      more.textContent = state.marketLoading ? "拉取中…" : "加载更多";
    } else {
      stat.textContent = "";
      more.classList.add("hidden");
    }

    // 一页不够一屏时自动接着拉，别让人对着空白干等
    if (state.view === "market") setTimeout(marketFillCheck, 220);
  }

  function showMarketError(text) {
    state.marketNote = text || "";
    if (!text) {
      elMarketError.classList.add("hidden");
      elMarketError.textContent = "";
      return;
    }
    elMarketError.classList.remove("hidden");
    elMarketError.textContent = text;
  }

  function setView(view) {
    state.view = view === "market" ? "market" : "local";
    elViewLocal.classList.toggle("hidden", state.view !== "local");
    elViewMarket.classList.toggle("hidden", state.view !== "market");
    document.getElementById("tab-local").classList.toggle("on", state.view === "local");
    document.getElementById("tab-market").classList.toggle("on", state.view === "market");
    elSearch.classList.toggle("hidden", state.view !== "local");
    if (state.view === "market") {
      renderMarket();
      if (!state.marketLoaded) send({ cmd: "market" });
    } else {
      render();
    }
  }

  // ------------------------------------------------------------ 消息处理
  function handleMessage(data) {
    if (typeof data === "string") {
      try { data = JSON.parse(data); } catch (e) { return; }
    }
    if (!data || !data.type) return;

    switch (data.type) {
      case "tools":
        state.tools = data.items || [];
        render();
        if (state.view === "market") renderMarket();
        showWarnings(data.warnings);
        break;

      case "market":
        state.market = data.items || [];
        state.marketTotal = data.total || state.market.length;
        state.marketOffset = data.offset || state.market.length;
        state.marketLoading = false;
        if (!data.error) state.marketLoaded = true;
        state.marketBusy = {};
        showMarketError(data.error || "");
        renderMarket();
        break;

      case "marketLoading":
        state.marketLoading = !!data.busy;
        renderMarket();
        break;

      case "marketBusy":
        if (data.busy) state.marketBusy[data.id] = true;
        else delete state.marketBusy[data.id];
        renderMarket();
        break;

      case "importStart":
        log("开始安装 " + (data.source || ""));
        break;

      case "import":
        log((data.ok ? "已安装工具 " : "安装失败 ") + (data.id || "") +
            (data.message ? "（" + data.message + "）" : ""));
        if (!data.ok && data.id) delete state.marketBusy[data.id];
        if (data.ok) {
          state.marketBusy = {};
          if (state.view === "market") send({ cmd: "market" });
        }
        break;

      case "busy":
        state.busy[data.id] = data.busy ? data.kind : null;
        if (!data.busy) delete state.busy[data.id];
        render();
        break;

      case "check": {
        state.status[data.id] = data.installed ? "ok" : "miss";
        var name = (state.tools.find(function (t) { return t.id === data.id; }) || {}).name || data.id;
        log(name + " → " + (data.installed ? "就绪" + " (" + data.exitCode + ")"
                                                       : "缺环境 (" + data.exitCode + ")"));
        if (data.output) log(data.output.trim());
        render();
        break;
      }

      case "install_start":
        log(data.command);
        break;

      case "install": {
        var okName = (state.tools.find(function (t) { return t.id === data.id; }) || {}).name || data.id;
        log(okName + " 安装" + (data.ok ? "完成" : "失败") + " (" + data.exitCode + ")");
        if (data.output) log(data.output.trim());
        break;
      }

      case "help":
        elModalTitle.textContent = data.name || data.id;
        elModalSub.textContent = "tools/" + data.id + "/help.md";
        elModalBody.innerHTML = markdownToHtml(data.markdown || "");
        elModal.classList.remove("hidden");
        break;

      case "action":
        log((data.ok ? "启动 " : "启动失败 ") + (data.label || ""));
        break;

      case "hidden":
        log("已收进托盘");
        break;

      case "openFolder":
        log(data.path || "");
        break;

      case "appInfo":
        if (data.version) elVersion.textContent = "v" + data.version;
        break;

      case "dragHover":
        setDropMask(!!data.on);
        break;

      case "publish":
        publishBusy = false;
        if (!data.ok && data.message === "没有选择文件夹") {
          pendingOpen = false;
          elModal.classList.add("hidden");
          break;
        }
        log(data.ok ? ("已暂存待审：" + (data.id || "") + " " + (data.version || ""))
                    : ("提交失败：" + (data.message || "")));
        showPending(data.message || (data.ok ? "已暂存" : "提交失败"), !!data.ok);
        break;

      case "pending":
        state.pending = data.items || [];
        state.pendingDir = data.dir || "";
        renderPending();
        break;
    }
  }

  if (bridge) {
    bridge.addEventListener("message", function (event) { handleMessage(event.data); });
  }

  // ------------------------------------------------------------ 发布（暂存待审）
  var publishBusy = false;
  var pendingOpen = false;

  function fmtSize(n) {
    n = Number(n) || 0;
    if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
    if (n >= 1024) return (n / 1024).toFixed(0) + " KB";
    return n + " B";
  }

  function fmtTime(ts) {
    if (!ts) return "";
    var d = new Date(Number(ts) * 1000);
    function p(x) { return (x < 10 ? "0" : "") + x; }
    return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) +
           " " + p(d.getHours()) + ":" + p(d.getMinutes());
  }

  function setDropMask(on) {
    var el = document.getElementById("dropmask");
    if (el) el.classList.toggle("hidden", !on);
  }

  function renderPending() {
    if (!pendingOpen) return;
    var items = state.pending || [];
    var html = "";
    if (!items.length) {
      html = '<p class="pj-empty">待审区还是空的。点「发布」选中一个带 tool.json 的工具文件夹，会打包存到这里。</p>';
    } else {
      html = '<div class="pj-list">';
      for (var i = 0; i < items.length; i++) {
        var p = items[i];
        html += '<div class="pj-row">' +
                  '<div class="pj-main">' +
                    '<div><b>' + escapeHtml(p.name || p.id) + '</b>' +
                    ' <span class="pj-ver">v' + escapeHtml(p.version || "") + '</span></div>' +
                    '<div class="pj-sub">' + escapeHtml(p.file) + ' · ' + fmtSize(p.size) +
                    (p.created ? ' · ' + fmtTime(p.created) : '') + '</div>' +
                  '</div>' +
                  '<button class="link pj-del" data-file="' + escapeHtml(p.file) + '">删除</button>' +
                '</div>';
      }
      html += '</div>';
    }
    html += '<div class="pj-foot">' +
              '<button class="btn" id="pj-open">打开待审目录</button>' +
              '<span class="pj-hint">' + escapeHtml(state.pendingDir || "") + '</span>' +
            '</div>' +
            '<p class="pj-tip">待审包不会自动上架：把它发给 minifan 审核，通过后才会进市场。</p>';
    elModalBody.innerHTML = html;
  }

  function showPending(msg, ok) {
    pendingOpen = true;
    elModalTitle.textContent = ok ? "已提交待审核" : "提交没成功";
    elModalSub.textContent = msg || "";
    elModal.classList.remove("hidden");
    renderPending();
    send({ cmd: "pendingList" });
  }

  document.getElementById("btn-publish").addEventListener("click", function () {
    if (publishBusy) { log("上一次还在打包，稍等"); return; }
    publishBusy = true;
    pendingOpen = true;
    elModalTitle.textContent = "提交审核";
    elModalSub.textContent = "选一个工具文件夹，打包后存进本机待审区";
    elModalBody.innerHTML = '<p class="pj-empty">正在等选择文件夹…选好后开始打包。</p>';
    elModal.classList.remove("hidden");
    send({ cmd: "publishTool" });
  });

  elModalBody.addEventListener("click", function (e) {
    var t = e.target;
    if (!t) return;
    if (t.id === "pj-open") {
      send({ cmd: "openFolder", target: "pending" });
      return;
    }
    var file = t.getAttribute && t.getAttribute("data-file");
    if (file && t.classList.contains("pj-del")) {
      if (!confirm("删掉待审包 " + file + "？")) return;
      send({ cmd: "pendingDelete", file: file });
    }
  });

  // ------------------------------------------------------------ 交互绑定
  document.getElementById("btn-refresh").addEventListener("click", function () {
    log("刷新：重扫本机工具 + 重拉线上清单");
    send({ cmd: "refresh" });
    // 本机工具重扫之外，线上清单也绕过缓存重新拉一遍
    if (!state.marketLoading) {
      state.marketLoading = true;
      renderMarket();
    }
    send({ cmd: "marketRefresh" });
  });

  document.getElementById("btn-folder").addEventListener("click", function () {
    send({ cmd: "openFolder", target: "tools" });
  });

  document.getElementById("btn-hide").addEventListener("click", function () {
    send({ cmd: "hide" });
  });

  document.getElementById("btn-import").addEventListener("click", function () {
    send({ cmd: "importFile" });
  });

  document.getElementById("tab-local").addEventListener("click", function () { setView("local"); });
  document.getElementById("tab-market").addEventListener("click", function () { setView("market"); });

  document.getElementById("btn-market-more").addEventListener("click", function () {
    requestMoreMarket();
  });

  if (elContent) {
    elContent.addEventListener("scroll", function () { marketFillCheck(); }, { passive: true });
  }

  document.getElementById("btn-market-refresh").addEventListener("click", function () {
    state.marketLoading = true;
    state.marketLoaded = false;
    log("拉工具清单");
    send({ cmd: "marketRefresh" });
    renderMarket();
  });

  document.getElementById("btn-clear").addEventListener("click", function () {
    elLog.textContent = "";
    if (elConsole) elConsole.classList.add("hidden");
  });

  document.getElementById("modal-close").addEventListener("click", function () {
    elModal.classList.add("hidden");
    pendingOpen = false;
  });

  elModal.addEventListener("click", function (e) {
    if (e.target === elModal) {
      elModal.classList.add("hidden");
      pendingOpen = false;
    }
  });

  elSearch.addEventListener("input", function () {
    state.filter = elSearch.value;
    render();
  });

  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") {
      elModal.classList.add("hidden");
      pendingOpen = false;
    }
  });


  // ------------------------------------------------------------ 拖放
  // 主路径：宿主关掉了 WebView2 的 AllowExternalDrop，文件拖进来由主窗口收 WM_DROPFILES，
  // 能拿到真实路径——文件夹、一次拖多个、多大的包都行（见 gui.cpp）。
  // 这里保留页面内的 HTML5 drop 作兜底：万一某些 WebView2 版本仍然把事件交给页面，
  // 就按 base64 传给宿主解压，只够小 zip 用。
  var elDropMask = document.getElementById("dropmask");
  var dragDepth = 0;

  window.addEventListener("dragenter", function (e) {
    e.preventDefault();
    dragDepth++;
    if (!elDropMask) return;
    var hasFiles = e.dataTransfer && e.dataTransfer.types &&
                   e.dataTransfer.types.indexOf("Files") >= 0;
    if (hasFiles) elDropMask.classList.remove("hidden");
  });

  window.addEventListener("dragover", function (e) {
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
  });

  window.addEventListener("dragleave", function (e) {
    e.preventDefault();
    dragDepth = Math.max(0, dragDepth - 1);
    if (dragDepth === 0 && elDropMask) elDropMask.classList.add("hidden");
  });

  function sendOneFile(file) {
    log("读取 " + file.name);
    var reader = new FileReader();
    reader.onload = function () {
      var result = String(reader.result || "");
      var comma = result.indexOf(",");
      send({ cmd: "importData", name: file.name, data: comma >= 0 ? result.slice(comma + 1) : "" });
    };
    reader.onerror = function () { log("读取失败：" + file.name); };
    reader.readAsDataURL(file);
  }

  window.addEventListener("drop", function (e) {
    e.preventDefault();
    dragDepth = 0;
    if (elDropMask) elDropMask.classList.add("hidden");
    var files = e.dataTransfer ? e.dataTransfer.files : null;
    if (!files || !files.length) return;

    var zips = [];
    for (var i = 0; i < files.length; i++) {
      if (/\.zip$/i.test(files[i].name)) {
        if (files[i].size > 32 * 1024 * 1024) { log("包太大了（兜底通道限 32 MB）：" + files[i].name); continue; }
        zips.push(files[i]);
      } else {
        log("只认 .zip 工具包：" + files[i].name);
      }
    }
    if (!zips.length) {
      log("拖文件夹请直接拖到窗口上（宿主会收路径）；这里是兜底通道，只认 zip");
      return;
    }
    for (var j = 0; j < zips.length; j++) sendOneFile(zips[j]);
  });

  send({ cmd: "ready" });
})();



