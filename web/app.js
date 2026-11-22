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

    var head = document.createElement("div");
    head.className = "card-head";
    var left = document.createElement("div");
    var title = document.createElement("h3");
    title.className = "card-title";
    title.textContent = tool.name || tool.id;
    var idEl = document.createElement("span");
    idEl.className = "card-id";
    idEl.textContent = tool.id;
    left.appendChild(title);
    left.appendChild(idEl);
    if (tool.version) {
      var ver = document.createElement("span");
      ver.className = "card-ver";
      ver.textContent = "  v" + tool.version;
      left.appendChild(ver);
    }

    var chip = document.createElement("span");
    var c = statusChip(tool.id);
    chip.className = "chip " + c.cls;
    chip.textContent = c.text;

    head.appendChild(left);
    head.appendChild(chip);

    var desc = document.createElement("p");
    desc.className = "card-desc";
    desc.textContent = tool.description || "（该工具没有写描述）";

    card.appendChild(head);
    card.appendChild(desc);

    var actions = document.createElement("div");
    actions.className = "card-actions";

    var busy = !!state.busy[tool.id];

    if (tool.hasCheck) {
      var bCheck = makeButton("检测", "ghost", function () {
        log("检测 " + tool.id + " …");
        send({ cmd: "check", id: tool.id });
      });
      bCheck.disabled = busy;
      actions.appendChild(bCheck);
    }
    if (tool.hasInstall) {
      var bInstall = makeButton("安装", "primary", function () {
        log("开始安装 " + tool.id + " …（可能需要几分钟）");
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
      actions.appendChild(makeButton(label, "", function () {
        send({ cmd: "action", id: tool.id, index: index });
      }));
    });

    card.appendChild(actions);
    return card;
  }

  function render() {
    var q = state.filter.trim().toLowerCase();
    var visible = state.tools.filter(function (t) {
      if (!q) return true;
      return (t.id + " " + t.name + " " + (t.description || "")).toLowerCase().indexOf(q) >= 0;
    });

    elGrid.textContent = "";
    visible.forEach(function (t) { elGrid.appendChild(renderCard(t)); });
    elCounter.textContent = visible.length + " / " + state.tools.length + " 个工具";
    elEmpty.classList.toggle("hidden", state.tools.length > 0);
  }

  function showWarnings(list) {
    if (!list || !list.length) {
      elWarn.classList.add("hidden");
      elWarn.textContent = "";
      return;
    }
    elWarn.classList.remove("hidden");
    elWarn.textContent = "有 " + list.length + " 个工具被忽略：\n· " + list.join("\n· ");
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
        log("检测 " + name + " → " + (data.installed ? "环境就绪（退出码 " + data.exitCode + "）"
                                                       : "缺少环境（退出码 " + data.exitCode + "）"));
        if (data.output) log(data.output.trim());
        render();
        break;
      }

      case "install_start":
        log("执行安装指令：" + data.command);
        break;

      case "install": {
        var okName = (state.tools.find(function (t) { return t.id === data.id; }) || {}).name || data.id;
        log(okName + " 安装" + (data.ok ? "完成" : "失败") + "（退出码 " + data.exitCode + "）");
        if (data.output) log(data.output.trim());
        break;
      }

      case "help":
        elModalTitle.textContent = (data.name || data.id) + " · 帮助";
        elModalSub.textContent = "tools/" + data.id + "/help.md";
        elModalBody.innerHTML = markdownToHtml(data.markdown || "");
        elModal.classList.remove("hidden");
        break;

      case "action":
        log(data.ok ? ("已启动：" + (data.label || "")) : ("启动失败：" + (data.label || "")));
        break;

      case "hidden":
        log("已隐藏到系统托盘，点击托盘图标可以重新打开。");
        break;

      case "openFolder":
        log("已打开目录：" + (data.path || ""));
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
    log("重新扫描 tools 目录…");
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

  log("界面已就绪，正在读取工具列表…");
  send({ cmd: "ready" });
})();
