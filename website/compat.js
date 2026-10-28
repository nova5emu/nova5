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
  // `playable` games form the "100% jugables" category.
  const NOVA5 = {
    PPSA28997: { status: 'in-game', playable: true, title: 'God of War Sons of Sparta',
      note: 'Gameplay a 60 FPS estables, con sonido y control (Nova5 0.5.0 o posterior).' },
    PPSA21564: { status: 'in-game', note: 'Jugable a unos 16–17 FPS en el aterrizaje de Sky Garden (RTX + CPU Intel).' },
    PPSA03671: { note: 'Ya no se cierra al iniciar: Nova5 analiza sus shaders grandes sin desbordarse. Pendiente de jugar.' },
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
      `${g.nova5 ? '<b class="tag nova">Probado en Nova5</b>' : ''}</p>` +
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
      games = (data.games ?? []).map((g) => {
        const ours = NOVA5[g.titleId] ?? (g.allTitleIds ?? []).map((id) => NOVA5[id]).find(Boolean);
        return {
          title: g.title ?? g.titleId, titleId: g.titleId, ids: g.allTitleIds ?? [g.titleId],
          cover: g.cover, status: ours?.status ?? g.overall, nova5: Boolean(ours), playable: Boolean(ours?.playable),
          note: ours?.note,
        };
      }).sort((a, b) => (b.playable - a.playable) || (b.nova5 - a.nova5) || (rank[a.status] - rank[b.status]) ||
        a.title.localeCompare(b.title, 'es'));
      count();
      render();
    })
    .catch(() => {
      grid.innerHTML = '<p class="fine">No se pudo cargar la lista ahora mismo. Consúltala en <a href="https://kytyps5.github.io/compatibility">kytyps5.github.io</a>.</p>';
    });
})();
