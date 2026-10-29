// Nexo page: interactive replica of the app's console carousel, the systems list and the
// platform tabs. Data mirrors Nexo's own table of consoles (LEEME.md in its repository).
(() => {
  // Release links. Asset names must match the files attached to each GitHub release.
  const NEXO = {
    source: 'https://github.com/oscargaldamez137-sys/nexo',
    windows: 'https://github.com/oscargaldamez137-sys/nexo/releases/latest/download/Instalar-Nexo.exe',
    android: 'https://github.com/oscargaldamez137-sys/nexo/releases/latest/download/Nexo.apk',
  };
  document.querySelectorAll('[data-nexo]').forEach((a) => { a.href = NEXO[a.dataset.nexo]; });

  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;

  // format: shape of the game case on the shelf, as in the app.
  const SYSTEMS = [
    { id: 'retro', name: 'Retro', emu: 'RetroArch', accent: '#FF6B6B', format: 'cart', art: true,
      files: 'NES, SNES, Game Boy, Mega Drive, N64, PC Engine, Master System, Game Gear',
      needs: 'Tus ROMs. RetroArch y sus núcleos los instala Nexo; algunos sistemas piden una BIOS en la carpeta system.' },
    { id: 'ps2', name: 'PS2', emu: 'PCSX2', accent: '#6FA8FF', format: 'dvd', art: true,
      files: '.iso, .chd, .cso, .bin, .gz',
      needs: 'La BIOS extraída de tu PS2. Nexo la importa, la selecciona y prepara el primer arranque y el mando.' },
    { id: 'ps3', name: 'PS3', emu: 'RPCS3', accent: '#B9C2D3', format: 'bluray', art: true,
      files: 'Carpetas con EBOOT.BIN e ISO descifradas',
      needs: 'El firmware PS3UPDAT.PUP de la web oficial de Sony. Nexo abre RPCS3 para instalarlo.' },
    { id: 'psp', name: 'PSP', emu: 'PPSSPP', accent: '#D58CF0', format: 'umd', art: true,
      files: '.iso, .cso, .pbp, .chd, .elf',
      needs: 'Solo tus juegos. PPSSPP no necesita BIOS.' },
    { id: 'gamecube-wii', name: 'GameCube y Wii', emu: 'Dolphin', accent: '#9B7BFF', format: 'mini', art: true,
      files: '.iso, .gcm, .wbfs, .rvz, .ciso, .gcz, .wad',
      needs: 'Tus juegos. Nexo configura el mando de GameCube y el Wiimote con Nunchuk emulados.' },
    { id: 'ps4', name: 'PS4', emu: 'shadPS4', accent: '#4C8DFF', format: 'bluray', art: true,
      files: 'Juegos extraídos y paquetes PKG para instalar',
      needs: 'Los módulos del sistema de tu consola que pida cada juego, según la guía oficial de shadPS4.' },
    { id: 'switch', name: 'Switch', emu: 'Eden, Ryujinx, Citron o yuzu', accent: '#EF6B73', format: 'card', art: true,
      files: '.nsp, .xci, .nca, .nro',
      needs: 'Las claves prod.keys y el firmware de tu consola. Nexo los coloca en su sitio.' },
    { id: 'ps5', name: 'PS5', emu: 'Nova5', accent: '#7CF4FF', format: 'bluray', art: true, tag: 'Experimental',
      files: 'Carpetas con eboot.bin y param.json',
      needs: 'Tus juegos de PS5 extraídos. Nexo instala y abre Nova5.' },
    { id: 'gbc', name: 'Game Boy Color', emu: 'mGBA', accent: '#56C596', format: 'cart',
      files: '.gbc, .gb', needs: 'Solo tus ROMs.' },
    { id: 'gba', name: 'Game Boy Advance', emu: 'mGBA', accent: '#8B7CF6', format: 'cart',
      files: '.gba', needs: 'Solo tus ROMs. La BIOS de GBA es opcional.' },
    { id: 'xbox', name: 'Xbox', emu: 'xemu', accent: '#7BD389', format: 'dvd',
      files: '.iso en formato XISO',
      needs: 'MCPX y una BIOS flash compatibles con xemu. El disco duro inicial viene con la descarga de xemu.' },
    { id: 'xbox360', name: 'Xbox 360', emu: 'Xenia Canary', accent: '#A3E635', format: 'dvd', tag: 'Experimental',
      files: '.iso, .xex y juegos GOD',
      needs: 'Solo tus juegos completos, con sus carpetas .data. No necesita BIOS.' },
  ];
  const FORMAT = { cart: 'Cartucho', dvd: 'Caja de DVD', bluray: 'Caja de Blu-ray', umd: 'UMD', mini: 'Minidisco', card: 'Tarjeta de juego' };

  // ---- Carousel ----
  const stage = document.getElementById('stage');
  const carousel = document.getElementById('carousel');
  const shelf = stage.querySelector('[data-shelf]');
  const dots = stage.querySelector('[data-dots]');
  const slides = SYSTEMS.filter((s) => s.art);
  let index = slides.findIndex((s) => s.id === 'ps5');

  slides.forEach((s, i) => {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'nx-slide';
    item.tabIndex = -1;
    item.setAttribute('aria-label', s.name);
    item.innerHTML = `<img src="assets/nexo/${s.id}.webp" alt="" width="640" height="480" draggable="false"${i === index ? '' : ' loading="lazy"'}>`;
    item.addEventListener('click', () => go(i, true));
    carousel.appendChild(item);

    const dot = document.createElement('button');
    dot.type = 'button';
    dot.className = 'nx-dot';
    dot.setAttribute('aria-label', s.name);
    dot.addEventListener('click', () => go(i, true));
    dots.appendChild(dot);
  });
  const items = [...carousel.children];
  const dotItems = [...dots.children];

  function fillShelf(s) {
    shelf.dataset.format = s.format;
    shelf.innerHTML = '';
    for (let i = 0; i < 6; i++) {
      const box = document.createElement('span');
      box.className = `nx-box nx-${s.format}`;
      box.style.setProperty('--i', i);
      box.style.setProperty('--tone', `${(i * 37) % 60 - 30}deg`);
      shelf.appendChild(box);
    }
  }

  function go(i, user) {
    if (user) stopAuto();
    index = (i + slides.length) % slides.length;
    const n = slides.length;
    items.forEach((el, j) => {
      let off = j - index;
      if (off > n / 2) off -= n;
      if (off < -n / 2) off += n;
      el.style.setProperty('--off', off);
      el.classList.toggle('on', off === 0);
      el.classList.toggle('far', Math.abs(off) > 2);
      el.setAttribute('aria-hidden', off === 0 ? 'false' : 'true');
    });
    dotItems.forEach((d, j) => d.setAttribute('aria-pressed', String(j === index)));
    const s = slides[index];
    stage.style.setProperty('--accent', s.accent);
    stage.querySelector('[data-name]').textContent = s.name;
    stage.querySelector('[data-emu]').innerHTML = `Se juega con ${s.emu}${s.tag ? ` <span class="nx-sys-tag">${s.tag}</span>` : ''}`;
    stage.querySelector('[data-formats]').innerHTML = `<b>${FORMAT[s.format]}</b> ${s.files}`;
    fillShelf(s);
  }

  stage.querySelectorAll('[data-step]').forEach((b) =>
    b.addEventListener('click', () => go(index + Number(b.dataset.step), true)));
  stage.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowRight') { e.preventDefault(); go(index + 1, true); }
    if (e.key === 'ArrowLeft') { e.preventDefault(); go(index - 1, true); }
  });

  // Drag or swipe, with a threshold so a tap still selects.
  let startX = null;
  let dragged = false;
  carousel.addEventListener('pointerdown', (e) => { startX = e.clientX; dragged = false; });
  addEventListener('pointerup', (e) => {
    if (startX === null) return;
    const dx = e.clientX - startX;
    startX = null;
    dragged = Math.abs(dx) > 40;
    if (dragged) go(index + (dx < 0 ? 1 : -1), true);
  });
  carousel.addEventListener('click', (e) => {
    if (dragged) { e.preventDefault(); e.stopPropagation(); dragged = false; }
  }, true);

  // Gentle autoplay until the visitor takes over; paused while off screen.
  let timer = null;
  let auto = !still;
  function startAuto() { if (auto && !timer) timer = setInterval(() => go(index + 1, false), 3600); }
  function stopAuto() { auto = false; clearInterval(timer); timer = null; }
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([entry]) => {
      if (entry.isIntersecting) startAuto(); else { clearInterval(timer); timer = null; }
    }, { threshold: 0.4 }).observe(stage);
  }
  stage.addEventListener('pointerenter', () => { clearInterval(timer); timer = null; });
  stage.addEventListener('pointerleave', startAuto);
  stage.addEventListener('focusin', () => { clearInterval(timer); timer = null; });

  const clock = stage.querySelector('[data-clock]');
  const tick = () => { clock.textContent = new Date().toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' }); };
  tick();
  setInterval(tick, 15000);

  go(index, false);

  // ---- Systems list ----
  const list = document.querySelector('[data-systems]');
  for (const s of SYSTEMS) {
    const d = document.createElement('details');
    d.className = 'nx-system spot';
    d.style.setProperty('--accent', s.accent);
    d.innerHTML =
      `<summary><span class="nx-sys-dot" aria-hidden="true"></span><span class="nx-sys-name">${s.name}</span>` +
      `<span class="nx-sys-emu">${s.emu}</span>${s.tag ? `<span class="nx-sys-tag">${s.tag}</span>` : ''}</summary>` +
      `<dl><div><dt>Formato</dt><dd>${FORMAT[s.format]}</dd></div>` +
      `<div><dt>Reconoce</dt><dd>${s.files}</dd></div>` +
      `<div><dt>Lo que aportas tú</dt><dd>${s.needs}</dd></div></dl>`;
    list.appendChild(d);
  }

  // ---- Platform tabs (WAI-ARIA tabs pattern) ----
  const tabs = [...document.querySelectorAll('.nx-tablist [role="tab"]')];
  const ink = document.querySelector('.nx-tab-ink');
  function select(tab, focus) {
    tabs.forEach((t) => {
      const on = t === tab;
      t.setAttribute('aria-selected', String(on));
      t.tabIndex = on ? 0 : -1;
      document.getElementById(t.getAttribute('aria-controls')).hidden = !on;
    });
    ink.style.transform = `translateX(${tab.offsetLeft - 4}px)`;
    ink.style.width = `${tab.offsetWidth}px`;
    if (focus) tab.focus();
  }
  tabs.forEach((t, i) => {
    t.addEventListener('click', () => select(t, false));
    t.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      e.preventDefault();
      select(tabs[(i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length], true);
    });
  });
  select(tabs[0], false);
  addEventListener('resize', () => select(tabs.find((t) => t.getAttribute('aria-selected') === 'true'), false));
})();
