(function () {
  const W = 684, H = 1216;
  function fit() {
    document.querySelectorAll('.shots .dev').forEach(function (dev) {
      const s = dev.clientWidth / W;
      dev.firstElementChild.style.transform = 'scale(' + s + ')';
    });
  }
  window.addEventListener('resize', fit);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(fit);
  fit();

  const rest = document.getElementById('m-rest');
  const bw = document.getElementById('m-bw');
  function setMode(oneBit) {
    document.body.classList.toggle('bw', oneBit);
    rest.classList.toggle('on', !oneBit); rest.setAttribute('aria-pressed', String(!oneBit));
    bw.classList.toggle('on', oneBit); bw.setAttribute('aria-pressed', String(oneBit));
  }
  rest.addEventListener('click', function () { setMode(false); });
  bw.addEventListener('click', function () { setMode(true); });

  const dlg = document.getElementById('zoom');
  const box = document.getElementById('zbox');
  function open(dev) {
    const scr = dev.firstElementChild.cloneNode(true);
    const s = Math.min((window.innerHeight - 48) / H, (window.innerWidth - 32) / W, 1);
    scr.style.transform = 'scale(' + s + ')';
    box.style.width = Math.round(W * s) + 'px';
    box.style.height = Math.round(H * s) + 'px';
    box.replaceChildren(scr);
    if (dlg.showModal) dlg.showModal();
  }
  document.querySelectorAll('.shots .dev').forEach(function (dev) {
    dev.tabIndex = 0;
    dev.setAttribute('role', 'button');
    dev.setAttribute('aria-label', '放大查看');
    dev.addEventListener('click', function () { open(dev); });
    dev.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); open(dev); } });
  });
  document.getElementById('zclose').addEventListener('click', function () { dlg.close(); });
  dlg.addEventListener('click', function (e) { if (e.target === dlg) dlg.close(); });
})();
