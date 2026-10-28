// Compatibility list: community reports from KytyPS5 (the core Nova5 builds on), loaded live, with
// the results we verified ourselves on Nova5 on top.
(() => {
  const SOURCE = 'https://kytyps5.github.io/data/compat-index.json';
  const PAGE = 48;
  const STATUS = {
    'in-game': { label: 'En juego', css: 'ok' },
    'main-menu': { label: 'Menú', css: 'menu' },
    'logo': { label: 'Logo', css: 'mid' },
    'doesnt-boot': { label: 'No arranca', css: 'bad' },
  };
  // Tested on Nova5 by us; these override the community status when we know better.
  // `playable` games form the "100% jugables" category. `community` ones were reported fully
  // playable by the KytyPS5 community (issue number in the note) but not yet checked on Nova5.
  const report = (n) => `Informe de la comunidad KytyPS5 #${n}`;
  const NOVA5 = {
    PPSA28997: { status: 'in-game', playable: true, title: 'God of War Sons of Sparta',
      note: 'Gameplay a 60 FPS estables, con sonido y control (Nova5 0.5.0 o posterior).' },
    PPSA21564: { status: 'in-game', note: 'Jugable a unos 16–17 FPS en el aterrizaje de Sky Garden (RTX + CPU Intel).' },
    PPSA03671: { note: 'Ya no se cierra al iniciar: Nova5 analiza sus shaders grandes sin desbordarse. Pendiente de jugar.' },
    PPSA14053: { status: 'in-game', playable: true, community: true, title: 'Afterimage', note: `60 FPS perfectos, audio correcto y sin fallos gráficos. ${report(529)}.` },
    PPSA20052: { status: 'in-game', playable: true, community: true, title: 'Worms Armageddon: Anniversary Edition', note: `60 FPS sólidos, sin fallos de audio ni gráficos. ${report(514)}.` },
    PPSA24643: { status: 'in-game', playable: true, community: true, title: 'The Messenger', note: `Unos 60 FPS; imagen, controles y audio correctos. ${report(311)}.` },
    PPSA15065: { status: 'in-game', playable: true, community: true, title: 'Cat Quest III', note: `Unos 60 FPS, sin fallos de imagen ni de audio. ${report(309)}.` },
    PPSA34959: { status: 'in-game', playable: true, community: true, title: 'The King is Watching', note: `Totalmente jugable a 60 FPS. ${report(1459)}.` },
    PPSA17837: { status: 'in-game', playable: true, community: true, title: 'Ib', note: `Funciona perfectamente, con FPS estables. ${report(1921)}.` },
    PPSA09076: { status: 'in-game', playable: true, community: true, title: 'DREDGE', note: `Funciona perfectamente, con FPS altos y estables. ${report(1607)}.` },
    PPSA03733: { status: 'in-game', playable: true, community: true, title: 'Doki Doki Literature Club Plus!', note: `Todo funciona, hasta 60 FPS; audio y logros correctos. ${report(1889)}.` },
    PPSA25872: { status: 'in-game', playable: true, community: true, title: 'Terminator 2D: NO FATE', note: `Partida nueva y se juega perfectamente. ${report(216)}.` },
    PPSA08576: { status: 'in-game', playable: true, community: true, title: 'Asterix & Obelix Slap them All!', note: `Entra en juego sin problemas y con rendimiento estable. ${report(568)}.` },
    PPSA28068: { status: 'in-game', playable: true, community: true, title: 'Mortal Kombat: Legacy Kollection', note: `Funciona perfectamente, sin problemas. ${report(623)}.` },
    PPSA03469: { status: 'in-game', playable: true, community: true, title: 'Blackwind', note: `Funciona bien, con todos los sonidos y a 60 FPS. ${report(263)}.` },
    PPSA30703: { status: 'in-game', playable: true, community: true, title: 'Vampire Crawlers', note: `Va fluido, con FPS estables. ${report(1616)}.` },
  };

  const grid = document.getElementById('compat-grid');
  const more = document.getElementById('compat-more');
  const search = document.getElementById('compat-search');
  const chips = [...document.querySelectorAll('.stat-chip')];
  let games = [];
  let filter = 'all';
  let shown = PAGE;

  const text = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
  const norm = (s) => String(s ?? '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();

  function visible() {
    const q = norm(search.value.trim());
    return games.filter((g) =>
      (filter === 'all' || (filter === 'playable' ? g.playable : g.status === filter)) &&
      (!q || norm(g.title).includes(q) || g.ids.some((id) => id.toLowerCase().includes(q))));
  }

  function card(g) {
    const s = STATUS[g.status] ?? STATUS.logo;
    const el = document.createElement('article');
    el.className = 'compat-card';
    el.innerHTML =
      `<div class="cover">${g.cover ? `<img loading="lazy" src="${text(g.cover)}" alt="">` : `<span>${text(g.title.slice(0, 1))}</span>`}</div>` +
      `<div class="info"><h3>${text(g.title)}</h3><p class="id">${text(g.titleId)}</p>` +
      `<p class="tags">${g.playable ? '<b class="tag full">100% jugable</b>' : `<b class="tag ${s.css}">${s.label}</b>`}` +
      `${g.community ? '<b class="tag comm">Jugable según la comunidad</b>' : g.nova5 ? '<b class="tag nova">Probado en Nova5</b>' : ''}</p>` +
      `${g.note ? `<p class="note">${text(g.note)}</p>` : ''}</div>`;
    return el;
  }

  function render() {
    const list = visible();
    grid.innerHTML = list.length ? '' : '<p class="fine">No hay juegos con ese filtro.</p>';
    const frag = document.createDocumentFragment();
    for (const g of list.slice(0, shown)) frag.appendChild(card(g));
    grid.appendChild(frag);
    more.hidden = list.length <= shown;
  }

  function count() {
    const counts = { all: games.length, playable: games.filter((g) => g.playable).length };
    for (const g of games) counts[g.status] = (counts[g.status] ?? 0) + 1;
    document.querySelectorAll('[data-count]').forEach((el) => { el.textContent = counts[el.dataset.count] ?? 0; });
  }

  chips.forEach((chip) => chip.addEventListener('click', () => {
    filter = chip.dataset.filter;
    chips.forEach((c) => c.classList.toggle('on', c === chip));
    shown = PAGE;
    render();
  }));
  search.addEventListener('input', () => { shown = PAGE; render(); });
  more.addEventListener('click', () => { shown += PAGE; render(); });

  fetch(SOURCE, { cache: 'no-cache' })
    .then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then((data) => {
      const rank = { 'in-game': 0, 'main-menu': 1, 'logo': 2, 'doesnt-boot': 3 };
      const matched = new Set();
      games = (data.games ?? []).map((g) => {
        const id = [g.titleId, ...(g.allTitleIds ?? [])].find((i) => NOVA5[i]);
        const ours = id && NOVA5[id];
        if (id) matched.add(id);
        return {
          title: g.title ?? g.titleId, titleId: g.titleId, ids: g.allTitleIds ?? [g.titleId],
          cover: g.cover, status: ours?.status ?? g.overall, nova5: Boolean(ours), playable: Boolean(ours?.playable),
          community: Boolean(ours?.community), note: ours?.note,
        };
      });
      // Our playable entries that the community index does not list yet.
      for (const [titleId, ours] of Object.entries(NOVA5)) {
        if (!ours.playable || matched.has(titleId)) continue;
        games.push({ title: ours.title, titleId, ids: [titleId], status: ours.status, nova5: true, playable: true,
          community: Boolean(ours.community), note: ours.note });
      }
      // Our own tests first, then community-playable games, then the rest.
      games.sort((a, b) => (b.playable - a.playable) || ((b.nova5 && !b.community) - (a.nova5 && !a.community)) ||
        (rank[a.status] - rank[b.status]) || a.title.localeCompare(b.title, 'es'));
      count();
      render();
    })
    .catch(() => {
      grid.innerHTML = '<p class="fine">No se pudo cargar la lista ahora mismo. Consúltala en <a href="https://kytyps5.github.io/compatibility">kytyps5.github.io</a>.</p>';
    });
})();
