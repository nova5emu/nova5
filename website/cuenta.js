// Shared by every page: who is signed in (through the reviews and comments box, giscus) and the star count of each
// "Ver código fuente" button.
(() => {
  const GISCUS = 'https://giscus.app';
  const SAVED = 'nova5-cuenta';

  const style = document.createElement('style');
  style.textContent = `
    .nv-account { display: inline-flex; align-items: center; gap: 8px; }
    .nv-account img { width: 26px; height: 26px; border-radius: 50%; object-fit: cover; box-shadow: 0 0 0 2px rgba(127, 127, 127, .35); }
    .gh-mark { width: 20px; height: 20px; fill: currentColor !important; stroke: none !important; flex: none; }
    .gh-stars { margin-left: 4px; padding: 1px 8px; border-radius: 999px; font-size: .85em; font-weight: 700; background: rgba(127, 127, 127, .18); }
  `;
  document.head.append(style);

  // ---- Signed in -----------------------------------------------------------------------------------------------
  const links = [...document.querySelectorAll('[data-account]')];
  const signedOut = links.map((a) => a.innerHTML);
  const read = () => { try { return JSON.parse(localStorage.getItem(SAVED) || 'null'); } catch { return null; } };
  const save = (user) => { try { user ? localStorage.setItem(SAVED, JSON.stringify(user)) : localStorage.removeItem(SAVED); } catch { /* private mode */ } };

  const show = (user) => {
    links.forEach((a, i) => {
      if (!user) {
        a.innerHTML = signedOut[i];
        a.removeAttribute('title');
        a.classList.remove('nv-account');
        return;
      }
      const img = document.createElement('img');
      img.src = user.avatarUrl + (user.avatarUrl.includes('?') ? '&' : '?') + 's=64';
      img.alt = '';
      img.width = 26;
      img.height = 26;
      const name = document.createElement('span');
      name.textContent = user.login;
      a.replaceChildren(img, name);
      a.classList.add('nv-account');
      a.title = `Sesión iniciada con GitHub como ${user.login}. Ir a reseñas y comentarios`;
    });
  };

  // Giscus keeps its session in this page's storage; without it (or after signing out) nobody is signed in.
  let session = false;
  try { session = !!localStorage.getItem('giscus-session') || new URL(location.href).searchParams.has('giscus'); } catch { /* private mode */ }
  if (!session) save(null);
  show(session ? read() : null);

  // The comments box says who is reading once it loads (data-emit-metadata), and when they sign out.
  window.addEventListener('message', (event) => {
    if (event.origin !== GISCUS || typeof event.data !== 'object' || !event.data || !event.data.giscus) return;
    const data = event.data.giscus;
    if (data.signOut) { save(null); show(null); return; }
    if (!('discussion' in data)) return;
    // Without a session giscus reads as its own bot (giscus[bot]): that is nobody signed in.
    let signedIn = false;
    try { signedIn = !!localStorage.getItem('giscus-session'); } catch { /* private mode */ }
    const person = signedIn && data.viewer && data.viewer.login && !data.viewer.login.endsWith('[bot]');
    const viewer = person ? { login: data.viewer.login, avatarUrl: data.viewer.avatarUrl } : null;
    save(viewer);
    show(viewer);
  });

  // ---- Stars ------------------------------------------------------------------------------------------------------
  const repos = new Map();
  document.querySelectorAll('[data-stars]').forEach((a) => {
    const repo = a.dataset.stars;
    if (!repos.has(repo)) repos.set(repo, []);
    repos.get(repo).push(a);
  });
  repos.forEach((buttons, repo) => {
    fetch(`https://api.github.com/repos/${repo}`, { headers: { Accept: 'application/vnd.github+json' } })
      .then((r) => (r.ok ? r.json() : Promise.reject(r.status)))
      .then((info) => {
        const count = info.stargazers_count || 0;
        buttons.forEach((a) => {
          const badge = document.createElement('span');
          badge.className = 'gh-stars';
          badge.textContent = count ? `★ ${count.toLocaleString('es')}` : '★';
          badge.title = 'Estrellas en GitHub';
          a.append(badge);
        });
      })
      .catch(() => {});
  });
})();
