// "Novedades" section: what each version brings, from novedades.json (or the file in data-src, like NovaS's
// novas-novedades.json; the same file the launcher shows after an update). The newest version is open; older ones
// fold into their own row.
(() => {
  const box = document.getElementById('changelog');
  if (!box) return;
  const day = (iso) => {
    const d = iso ? new Date(iso + 'T12:00:00') : null;
    return d && !isNaN(d) ? d.toLocaleDateString('es-ES', { day: 'numeric', month: 'long', year: 'numeric' }) : 'Próximamente';
  };
  const list = (items) => {
    const ul = document.createElement('ul');
    for (const text of items) {
      const li = document.createElement('li');
      li.textContent = text;
      ul.append(li);
    }
    return ul;
  };
  const head = (v) => {
    const p = document.createElement('p');
    p.className = 'ver';
    p.textContent = `Versión ${v.version} · ${day(v.date)}`;
    return p;
  };
  const repo = box.dataset.repo || 'nova5emu/nova5';
  fetch(box.dataset.src || 'novedades.json', { cache: 'no-cache' })
    .then((r) => (r.ok ? r.json() : Promise.reject()))
    .then(({ versions }) => {
      // Only released versions: the one being prepared has no date yet.
      const shown = versions.filter((v) => v.date);
      box.textContent = '';
      shown.forEach((v, i) => {
        if (i === 0) {
          const card = document.createElement('article');
          card.className = 'release';
          const h3 = document.createElement('h3');
          h3.textContent = v.title;
          card.append(head(v), h3, list(v.items));
          box.append(card);
          return;
        }
        const details = document.createElement('details');
        const summary = document.createElement('summary');
        const title = document.createElement('span');
        title.innerHTML = `<b></b> <span class="muted"></span>`;
        title.querySelector('b').textContent = `${v.version} · ${v.title}`;
        title.querySelector('.muted').textContent = day(v.date);
        summary.append(title);
        details.append(summary, list(v.items));
        box.append(details);
      });
    })
    .catch(() => {
      box.innerHTML = `<p class="fine">No se pudieron cargar las novedades. Están también en <a href="https://github.com/${repo}/releases">las versiones de GitHub</a>.</p>`;
    });
})();
