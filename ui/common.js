// platendoos tool windows: the bridge to the mod.
//   rbm.send(obj)         -> the mod's onMessage (as JSON)
//   rbm.onState = fn(obj) <- what the mod posts
//   rbm.esc(text)         -> HTML-escaped text; always use it (or textContent) for anything from tracks/files
//   rbm.syncRows(box, items, htmlOf, keyOf) -> fills a tbody (or any container) with one element per item, reusing the
//                            elements of an earlier call: htmlOf(item, i) is the row's markup (escape with rbm.esc), keyOf(item, i)
//                            its identity (default: the position, keys must be unique). A row whose markup is unchanged is left
//                            alone, a changed one only has its changed attributes and cells rewritten; rows are added, removed and moved.
//   rbm.undoKey = fn(redo) <- ⌘Z / ⇧⌘Z on macOS (core/webui_mac.mm catches them); return true if the page undid
//                            something itself, false to let a focused text field undo. Only ui/autocue.html sets it.
//                            Windows has no such hook: a page that wants Ctrl+Z / Ctrl+Y handles its own keydown (autocue.html does)
(function () {
  var wk = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.rbm;
  var wv = window.chrome && window.chrome.webview;
  var rbm = window.rbm = {
    onState: null,
    send: function (obj) {
      var s = JSON.stringify(obj);
      if (wk) wk.postMessage(s); else if (wv) wv.postMessage(s);
    },
    esc: function (t) {
      return String(t == null ? '' : t).replace(/[&<>"']/g, function (c) {
        return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
      });
    }
  };
  rbm.syncRows = function (box, items, htmlOf, keyOf) {
    var old = {}, i, el, next;
    for (el = box.firstElementChild; el; el = el.nextElementSibling) if (el.__k !== undefined) old[el.__k] = el;
    var scratch = document.createElement(box.tagName);  // the parsing context (a <tr> only parses inside a tbody)
    function parse(html) { scratch.innerHTML = html.trim(); return scratch.firstElementChild; }
    function sameAttrs(a, b) {  // make a's attributes those of b
      for (var k = a.attributes.length - 1; k >= 0; k--) if (!b.hasAttribute(a.attributes[k].name)) a.removeAttribute(a.attributes[k].name);
      for (k = 0; k < b.attributes.length; k++) if (a.getAttribute(b.attributes[k].name) !== b.attributes[k].value) a.setAttribute(b.attributes[k].name, b.attributes[k].value);
    }
    function patch(a, b) {
      sameAttrs(a, b);
      var ca = a.children, cb = [].slice.call(b.children), n;
      for (n = 0; n < cb.length; n++) {
        if (n >= ca.length) a.appendChild(cb[n]);
        else if (ca[n].outerHTML !== cb[n].outerHTML) { sameAttrs(ca[n], cb[n]); if (ca[n].innerHTML !== cb[n].innerHTML) ca[n].innerHTML = cb[n].innerHTML; }
      }
      while (ca.length > cb.length) a.removeChild(a.lastElementChild);
    }
    next = box.firstElementChild;
    for (i = 0; i < items.length; i++) {
      var key = keyOf ? keyOf(items[i], i) : i, html = htmlOf(items[i], i);
      el = old[key];
      delete old[key];  // a repeated key gets a new element
      if (!el) { el = parse(html); el.__k = key; el.__h = html; }
      else if (el.__h !== html) { patch(el, parse(html)); el.__h = html; }
      if (el === next) next = next.nextElementSibling; else box.insertBefore(el, next);
    }
    for (var k in old) if (old[k].parentNode === box) box.removeChild(old[k]);
    while (box.lastElementChild && box.lastElementChild.__k === undefined) box.removeChild(box.lastElementChild);
  };
  if (wv) wv.addEventListener('message', function (e) { if (rbm.onState) rbm.onState(e.data); });
  document.addEventListener('contextmenu', function (e) { e.preventDefault(); });
  // pages call rbm.ready() once their onState is set: the mod then (re)sends its state
  rbm.ready = function () { rbm.send({ cmd: 'ready' }); };
})();
