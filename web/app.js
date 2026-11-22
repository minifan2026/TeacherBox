// TeacherBox 前端逻辑：通过 WebMessage(JSON) 与 gui.exe 通信
(function () {
  "use strict";

  var bridge = window.chrome && window.chrome.webview ? window.chrome.webview : null;

  var state = {
    tools: [],
    status: {},   // id -> 'unknown' | 'ok' | 'miss'
    busy: {},     // id -> 'check' | 'install'
    filter: "",
    toolsDir: ""
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
        showWarnings(data.warnings);
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
    }
  }

  if (bridge) {
    bridge.addEventListener("message", function (event) { handleMessage(event.data); });
  }

  // ------------------------------------------------------------ 交互绑定
  document.getElementById("btn-refresh").addEventListener("click", function () {
    log("扫描");
    send({ cmd: "refresh" });
  });

  document.getElementById("btn-folder").addEventListener("click", function () {
    send({ cmd: "openFolder", target: "tools" });
  });

  document.getElementById("btn-hide").addEventListener("click", function () {
    send({ cmd: "hide" });
  });

  document.getElementById("btn-clear").addEventListener("click", function () {
    elLog.textContent = "";
    if (elConsole) elConsole.classList.add("hidden");
  });

  document.getElementById("modal-close").addEventListener("click", function () {
    elModal.classList.add("hidden");
  });

  elModal.addEventListener("click", function (e) {
    if (e.target === elModal) elModal.classList.add("hidden");
  });

  elSearch.addEventListener("input", function () {
    state.filter = elSearch.value;
    render();
  });

  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") elModal.classList.add("hidden");
  });


  send({ cmd: "ready" });
})();



