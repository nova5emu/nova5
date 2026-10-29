// Nexo page: a replica of the app's home screen (console cards, shelf of games drawn as their
// physical media), the systems list and the platform tabs. Colors, texts and media styles mirror
// Nexo's own Catalog.cs and MediaView.cs.
(() => {
  // Release links. Asset names must match the files attached to each GitHub release.
  const NEXO = {
    source: 'https://github.com/oscargaldamez137-sys/nexo',
    windows: 'https://github.com/oscargaldamez137-sys/nexo/releases/latest/download/Instalar-Nexo.exe',
    android: 'https://github.com/oscargaldamez137-sys/nexo/releases/latest/download/Nexo.apk',
  };
  document.querySelectorAll('[data-nexo]').forEach((a) => { a.href = NEXO[a.dataset.nexo]; });

  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const LIBRARY = {"retro":[{"title":"The Legend of Zelda","img":"assets/nexo/juegos/retro/the-legend-of-zelda.webp","w":252,"h":360},{"title":"Mega Man 2","img":"assets/nexo/juegos/retro/mega-man-2.webp","w":252,"h":360},{"title":"Super Mario Bros. 3","img":"assets/nexo/juegos/retro/super-mario-bros-3.webp","w":252,"h":360},{"title":"Donkey Kong Country","img":"assets/nexo/juegos/retro/donkey-kong-country.webp","w":512,"h":357},{"title":"The Legend of Zelda - A Link to the Past","img":"assets/nexo/juegos/retro/the-legend-of-zelda-a-link-to-the-past.webp","w":512,"h":357},{"title":"Super Mario World","img":"assets/nexo/juegos/retro/super-mario-world.webp","w":512,"h":357},{"title":"Super Metroid","img":"assets/nexo/juegos/retro/super-metroid.webp","w":512,"h":357},{"title":"The Legend of Zelda - Ocarina of Time","img":"assets/nexo/juegos/retro/the-legend-of-zelda-ocarina-of-time.webp","w":512,"h":357},{"title":"Mario Kart 64","img":"assets/nexo/juegos/retro/mario-kart-64.webp","w":512,"h":357},{"title":"Super Mario 64","img":"assets/nexo/juegos/retro/super-mario-64.webp","w":512,"h":357}],"gbc":[{"title":"The Legend of Zelda - Links Awakening DX","img":"assets/nexo/juegos/gbc/the-legend-of-zelda-links-awakening-dx.webp","w":360,"h":360},{"title":"Pokemon - Crystal Version","img":"assets/nexo/juegos/gbc/pokemon-crystal-version.webp","w":360,"h":360},{"title":"Pokemon - Gold Version","img":"assets/nexo/juegos/gbc/pokemon-gold-version.webp","w":360,"h":360}],"gba":[{"title":"The Legend of Zelda - The Minish Cap","img":"assets/nexo/juegos/gba/the-legend-of-zelda-the-minish-cap.webp","w":360,"h":360},{"title":"Mario Kart - Super Circuit","img":"assets/nexo/juegos/gba/mario-kart-super-circuit.webp","w":360,"h":360},{"title":"Metroid - Zero Mission","img":"assets/nexo/juegos/gba/metroid-zero-mission.webp","w":360,"h":360},{"title":"Pokemon - Emerald Version","img":"assets/nexo/juegos/gba/pokemon-emerald-version.webp","w":360,"h":360}],"ps2":[{"title":"Dragon Ball Z - Budokai Tenkaichi 3","img":"assets/nexo/juegos/ps2/dragon-ball-z-budokai-tenkaichi-3.webp","w":254,"h":360},{"title":"God of War","img":"assets/nexo/juegos/ps2/god-of-war.webp","w":254,"h":360},{"title":"Grand Theft Auto - San Andreas","img":"assets/nexo/juegos/ps2/grand-theft-auto-san-andreas.webp","w":255,"h":360},{"title":"Kingdom Hearts","img":"assets/nexo/juegos/ps2/kingdom-hearts.webp","w":254,"h":360},{"title":"Shadow of the Colossus","img":"assets/nexo/juegos/ps2/shadow-of-the-colossus.webp","w":255,"h":360}],"psp":[{"title":"Crisis Core - Final Fantasy VII","img":"assets/nexo/juegos/psp/crisis-core-final-fantasy-vii.webp","w":223,"h":360},{"title":"God of War - Chains of Olympus","img":"assets/nexo/juegos/psp/god-of-war-chains-of-olympus.webp","w":209,"h":360},{"title":"Grand Theft Auto - Vice City Stories","img":"assets/nexo/juegos/psp/grand-theft-auto-vice-city-stories.webp","w":209,"h":360}],"gc":[{"title":"The Legend of Zelda - The Wind Waker","img":"assets/nexo/juegos/gc/the-legend-of-zelda-the-wind-waker.webp","w":252,"h":360},{"title":"Metroid Prime","img":"assets/nexo/juegos/gc/metroid-prime.webp","w":257,"h":360},{"title":"Super Smash Bros. Melee","img":"assets/nexo/juegos/gc/super-smash-bros-melee.webp","w":259,"h":360}],"wii":[{"title":"Mario Kart Wii","img":"assets/nexo/juegos/wii/mario-kart-wii.webp","w":254,"h":360},{"title":"Super Mario Galaxy","img":"assets/nexo/juegos/wii/super-mario-galaxy.webp","w":256,"h":360},{"title":"Super Smash Bros. Brawl","img":"assets/nexo/juegos/wii/super-smash-bros-brawl.webp","w":256,"h":360}],"xbox":[{"title":"Fable","img":"assets/nexo/juegos/xbox/fable.webp","w":257,"h":360},{"title":"Halo - Combat Evolved","img":"assets/nexo/juegos/xbox/halo-combat-evolved.webp","w":251,"h":360},{"title":"Halo 2","img":"assets/nexo/juegos/xbox/halo-2.webp","w":257,"h":360},{"title":"Star Wars - Knights of the Old Republic","img":"assets/nexo/juegos/xbox/star-wars-knights-of-the-old-republic.webp","w":257,"h":360}],"ps3":[{"title":"Blur","img":"assets/nexo/juegos/ps3/blur.webp","w":309,"h":360},{"title":"Plants vs. Zombies: Garden Warfare","img":"assets/nexo/juegos/ps3/plants-vs-zombies-garden-warfare.webp","w":309,"h":360}],"ps4":[{"title":"Dragon Ball FighterZ","img":"assets/nexo/juegos/ps4/dragon-ball-fighterz.webp","w":360,"h":360},{"title":"Spyro Reignited Trilogy","img":"assets/nexo/juegos/ps4/spyro-reignited-trilogy.webp","w":360,"h":360}],"switch":[{"title":"Pokémon Sword","img":"assets/nexo/juegos/switch/pok-mon-sword.webp","w":360,"h":360}],"ps5":[{"title":"ASTRO BOT","img":"assets/nexo/juegos/ps5/astro-bot.webp","w":360,"h":360},{"title":"Demon's Souls","img":"assets/nexo/juegos/ps5/demon-s-souls.webp","w":360,"h":360},{"title":"God of War Sons of Sparta","img":"assets/nexo/juegos/ps5/god-of-war-sons-of-sparta.webp","w":360,"h":360},{"title":"Marvel's Wolverine","img":"assets/nexo/juegos/ps5/marvel-s-wolverine.webp","w":360,"h":360}]};

  // kind: physical media drawn on the shelf. box: plastic case colors (plastic, band, ink, brand).
  const SYSTEMS = [
    { id: 'retro', name: 'Retro', maker: 'Varios fabricantes', emu: 'RetroArch', kind: 'cart', primary: '#7C5CFC', secondary: '#241650',
      desc: 'Una puerta a muchas generaciones. Reúne distintos sistemas mediante núcleos que se configuran dentro de RetroArch.',
      files: 'NES, SNES, N64, Mega Drive, PC Engine, Master System, Game Gear', needs: 'Tus ROMs. RetroArch y sus núcleos los instala Nexo; algunos sistemas piden una BIOS en la carpeta system.' },
    { id: 'ps2', name: 'PlayStation 2', maker: 'Sony', emu: 'PCSX2', kind: 'case', primary: '#368CFF', secondary: '#102752',
      box: ['#16171B', '#050507', '#FFFFFF', 'PlayStation 2', true],
      desc: 'Vuelve a la generación de PlayStation 2 desde una biblioteca propia. Configura el emulador y sus controles a tu ritmo.',
      files: '.iso, .chd, .cso, .bin, .gz', needs: 'La BIOS extraída de tu PS2. Nexo la importa, la selecciona y prepara el primer arranque y el mando.' },
    { id: 'ps3', name: 'PlayStation 3', maker: 'Sony', emu: 'RPCS3', kind: 'case', primary: '#E85A87', secondary: '#46162F',
      box: ['#141519', '#0A0B0E', '#E6E8EE', 'PS3', false, true],
      desc: 'La generación de PlayStation 3 en tu PC. La compatibilidad y el rendimiento dependen del juego y del equipo.',
      files: 'Carpetas con EBOOT.BIN e ISO descifradas', needs: 'El firmware PS3UPDAT.PUP de la web oficial de Sony. Nexo abre RPCS3 para instalarlo.' },
    { id: 'psp', name: 'PSP', maker: 'Sony', emu: 'PPSSPP', kind: 'umd', primary: '#F4AD47', secondary: '#49301A',
      desc: 'El espíritu de una portátil, en una pantalla grande. Prepara PPSSPP y sus controles para tu próxima partida.',
      files: '.iso, .cso, .pbp, .chd, .elf', needs: 'Solo tus juegos. PPSSPP no necesita BIOS.' },
    { id: 'gamecube-wii', name: 'GameCube / Wii', maker: 'Nintendo', emu: 'Dolphin', kind: 'case', primary: '#19BDD0', secondary: '#083C49',
      box: ['#151519', '#050507', '#A696FF', 'GAMECUBE', false, false, true],
      boxWii: ['#EEF0F4', '#FFFFFF', '#8C919A', 'Wii', true],
      desc: 'Dos consolas, un mismo lugar. Explora tus juegos de GameCube y Wii con el emulador preparado a tu gusto.',
      files: '.iso, .gcm, .wbfs, .rvz, .ciso, .gcz, .wad', needs: 'Tus juegos. Nexo configura el mando de GameCube y el Wiimote con Nunchuk emulados.' },
    { id: 'ps4', name: 'PlayStation 4', maker: 'Sony', emu: 'shadPS4', kind: 'case', primary: '#467BFF', secondary: '#10204D', tag: 'Experimental',
      box: ['#0E2A66', '#0043B0', '#FFFFFF', 'PS4'],
      desc: 'Tus juegos de PlayStation 4, organizados como una colección de discos. La compatibilidad depende del emulador y del juego.',
      files: 'Juegos extraídos y paquetes PKG para instalar', needs: 'Los módulos del sistema de tu consola que pida cada juego, según la guía oficial de shadPS4.' },
    { id: 'switch', name: 'Nintendo Switch', maker: 'Nintendo', emu: 'Eden, Ryujinx, Citron o yuzu', kind: 'card', primary: '#EE5266', secondary: '#481B2C',
      desc: 'Una estantería de tarjetas para tus juegos de Switch. Copia tu emulador compatible y configura sus requisitos por separado.',
      files: '.nsp, .xci, .nca, .nro', needs: 'Las claves prod.keys y el firmware de tu consola. Nexo los coloca en su sitio.' },
    { id: 'ps5', name: 'PlayStation 5', maker: 'Sony', emu: 'Nova5', kind: 'case', primary: '#A7B6DC', secondary: '#222943', tag: 'Experimental',
      box: ['#1A3E8C', '#FAFBFD', '#101114', 'PS5'],
      desc: 'Nova5, nuestro emulador de PS5. Soporte experimental: mostrar un juego no garantiza que pueda ejecutarse.',
      files: 'Carpetas con eboot.bin y param.json', needs: 'Tus juegos de PS5 extraídos. Nexo instala y abre Nova5.' },
    { id: 'gbc', name: 'Game Boy Color', maker: 'Nintendo', emu: 'mGBA', kind: 'gb', primary: '#A874E8', secondary: '#332044',
      desc: 'Pequeños cartuchos, grandes aventuras. Tu colección de Game Boy y Game Boy Color con píxeles nítidos.',
      files: '.gbc, .gb', needs: 'Solo tus ROMs.' },
    { id: 'gba', name: 'Game Boy Advance', maker: 'Nintendo', emu: 'mGBA', kind: 'gba', primary: '#7268EE', secondary: '#242044',
      desc: 'Una portátil llena de color. Recupera tus aventuras de Game Boy Advance en una estantería de cartuchos.',
      files: '.gba', needs: 'Solo tus ROMs. La BIOS de GBA es opcional.' },
    { id: 'xbox', name: 'Xbox', maker: 'Microsoft', emu: 'xemu', kind: 'case', primary: '#69C62F', secondary: '#18341B',
      box: ['#121315', '#3E8E1F', '#FFFFFF', 'XBOX', false, true],
      desc: 'La primera generación de Xbox, en tu colección. Prepara los archivos de tu consola y consulta la compatibilidad de cada juego.',
      files: '.iso en formato XISO', needs: 'MCPX y una BIOS flash compatibles con xemu. El disco duro inicial viene con la descarga de xemu.' },
    { id: 'xbox360', name: 'Xbox 360', maker: 'Microsoft', emu: 'Xenia Canary', kind: 'case', primary: '#9CD342', secondary: '#293B20', tag: 'Experimental',
      box: ['#2F7D24', '#F7F8F9', '#5C9E2E', 'XBOX 360'],
      desc: 'Una nueva generación de juegos en cajas verdes. Xenia Canary es experimental: el resultado depende de cada título.',
      files: '.iso, .xex y juegos GOD', needs: 'Solo tus juegos completos, con sus carpetas .data. No necesita BIOS.' },
  ];
  const FORMAT = { cart: 'Cartucho', gb: 'Cartucho', gba: 'Cartucho', case: 'Caja con disco', umd: 'UMD', card: 'Tarjeta de juego' };
  const gamesOf = (s) => s.id === 'gamecube-wii'
    ? [...(LIBRARY.gc ?? []).map((g) => ({ ...g, wii: false })), ...(LIBRARY.wii ?? []).map((g) => ({ ...g, wii: true }))]
    : LIBRARY[s.id] ?? [];

  const esc = (t) => String(t).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);

  // ---- Media, drawn like MediaView.cs ----
  function media(s, g) {
    const art = `<img src="${esc(g.img)}" alt="" loading="lazy" width="${g.w}" height="${g.h}">`;
    const disc = `<span class="nx-disc" style="--art:url('${esc(g.img)}')"></span>`;
    switch (s.kind) {
      case 'case': {
        const [plastic, band, ink, brand, italic, sheen, mini] = g.wii ? s.boxWii : s.box;
        const boxArt = g.w / g.h < 0.95; // real box art already carries the console band
        return `<span class="nx-media nx-case${mini ? ' mini' : ''}" style="--plastic:${plastic};--band:${band};--ink:${ink}">${disc}` +
          `<span class="nx-shell">${boxArt ? '' : `<span class="nx-band${italic ? ' it' : ''}${sheen ? ' sheen' : ''}">${brand}</span>`}` +
          `<span class="nx-art">${art}</span></span></span>`;
      }
      case 'umd':
        return `<span class="nx-media nx-umd"><span class="nx-shell"><span class="nx-label">${art}</span>` +
          `<span class="nx-window">${disc}</span><b>UMD</b></span></span>`;
      case 'card':
        return `<span class="nx-media nx-card"><span class="nx-shell"><span class="nx-label">${art}</span></span></span>`;
      case 'gb':
        return `<span class="nx-media nx-gb"><span class="nx-shell"><span class="nx-label">${art}</span></span></span>`;
      case 'gba':
        return `<span class="nx-media nx-gba"><span class="nx-shell"><span class="nx-label">${art}</span></span></span>`;
      default:
        return `<span class="nx-media nx-cart"><span class="nx-shell"><span class="nx-label">${art}</span></span></span>`;
    }
  }

  // ---- Carousel of console cards ----
  const stage = document.getElementById('stage');
  const carousel = document.getElementById('carousel');
  const shelf = stage.querySelector('[data-shelf]');
  const dots = stage.querySelector('[data-dots]');
  let index = SYSTEMS.findIndex((s) => s.id === 'ps2');

  SYSTEMS.forEach((s, i) => {
    const n = gamesOf(s).length;
    const card = document.createElement('button');
    card.type = 'button';
    card.className = 'nx-console';
    card.tabIndex = -1;
    card.style.setProperty('--p', s.primary);
    card.style.setProperty('--s', s.secondary);
    card.setAttribute('aria-label', s.name);
    card.innerHTML =
      `<span class="nx-badge">${n ? `${n} juego${n === 1 ? '' : 's'}` : 'Sin juegos'}</span>` +
      `<img src="assets/nexo/consolas/${s.id}.webp" alt="" draggable="false" loading="${i === index ? 'eager' : 'lazy'}">` +
      `<span class="nx-console-name">${esc(s.name)}</span><span class="nx-console-emu">${esc(s.emu)}${s.tag ? ` · ${s.tag.toLowerCase()}` : ''}</span>`;
    card.addEventListener('click', () => go(i, true));
    carousel.appendChild(card);

    const dot = document.createElement('button');
    dot.type = 'button';
    dot.className = 'nx-dot';
    dot.setAttribute('aria-label', s.name);
    dot.addEventListener('click', () => go(i, true));
    dots.appendChild(dot);
  });
  const cards = [...carousel.children];
  const dotItems = [...dots.children];

  let selected = 0;
  function pick(i) {
    const items = [...shelf.children];
    if (!items.length) return;
    selected = (i + items.length) % items.length;
    items.forEach((el, j) => el.classList.toggle('on', j === selected));
    const el = items[selected];
    shelf.scrollTo({ left: el.offsetLeft - (shelf.clientWidth - el.offsetWidth) / 2, behavior: still ? 'auto' : 'smooth' });
  }

  function fillShelf(s) {
    const games = gamesOf(s);
    shelf.dataset.kind = s.kind;
    shelf.innerHTML = games.length
      ? games.map((g, i) => `<button type="button" role="listitem" class="nx-game" style="--i:${i}" aria-label="${esc(g.title)}">` +
          `${media(s, g)}<span class="nx-title">${esc(g.title)}</span></button>`).join('')
      : `<p class="nx-empty">Copia tus juegos en <b>Juegos\\${s.id === 'xbox360' ? 'Xbox360' : s.id}</b> y aparecerán aquí.</p>`;
    [...shelf.querySelectorAll('.nx-game')].forEach((b, i) => {
      b.addEventListener('click', () => { stopAuto(); pick(i); });
      b.addEventListener('pointerenter', () => { if (matchMedia('(hover: hover)').matches) pick(i); });
    });
    pick(0);
  }

  function go(i, user) {
    if (user) stopAuto();
    const n = SYSTEMS.length;
    index = (i + n) % n;
    cards.forEach((el, j) => {
      let off = j - index;
      if (off > n / 2) off -= n;
      if (off < -n / 2) off += n;
      el.style.setProperty('--off', off);
      el.style.setProperty('--abs', Math.abs(off));
      el.classList.toggle('on', off === 0);
      el.classList.toggle('far', Math.abs(off) > 2);
    });
    dotItems.forEach((d, j) => d.setAttribute('aria-pressed', String(j === index)));
    const s = SYSTEMS[index];
    stage.style.setProperty('--accent', s.primary);
    stage.style.setProperty('--deep', s.secondary);
    stage.querySelector('[data-kicker]').textContent = `${s.maker} · Emulador: ${s.emu}`;
    stage.querySelector('[data-name]').textContent = s.name;
    stage.querySelector('[data-desc]').textContent = s.desc;
    fillShelf(s);
  }

  stage.querySelectorAll('[data-step]').forEach((b) =>
    b.addEventListener('click', () => go(index + Number(b.dataset.step), true)));
  stage.addEventListener('keydown', (e) => {
    const k = { ArrowRight: () => go(index + 1, true), ArrowLeft: () => go(index - 1, true),
      ArrowDown: () => { stopAuto(); pick(selected + 1); }, ArrowUp: () => { stopAuto(); pick(selected - 1); } }[e.key];
    if (k) { e.preventDefault(); k(); }
  });

  // Swipe or drag the console cards, with a threshold so a tap still selects.
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

  // Gentle autoplay until the visitor takes over; paused while off screen or hovered.
  let timer = null;
  let auto = !still;
  function startAuto() { if (auto && !timer) timer = setInterval(() => go(index + 1, false), 4200); }
  function stopAuto() { auto = false; clearInterval(timer); timer = null; }
  const pause = () => { clearInterval(timer); timer = null; };
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([entry]) => { if (entry.isIntersecting) startAuto(); else pause(); }, { threshold: 0.4 }).observe(stage);
  }
  stage.addEventListener('pointerenter', pause);
  stage.addEventListener('pointerleave', startAuto);
  stage.addEventListener('focusin', pause);

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
    d.style.setProperty('--accent', s.primary);
    d.innerHTML =
      `<summary><img class="nx-sys-photo" src="assets/nexo/consolas/${s.id}.webp" alt="" loading="lazy">` +
      `<span class="nx-sys-name">${esc(s.name)}</span>` +
      `<span class="nx-sys-emu">${esc(s.emu)}</span>${s.tag ? `<span class="nx-sys-tag">${s.tag}</span>` : ''}</summary>` +
      `<dl><div><dt>Formato</dt><dd>${FORMAT[s.kind]}</dd></div>` +
      `<div><dt>Reconoce</dt><dd>${esc(s.files)}</dd></div>` +
      `<div><dt>Lo que aportas tú</dt><dd>${esc(s.needs)}</dd></div></dl>`;
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
