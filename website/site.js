// Shared motion and navigation for every page. Everything here is progressive: without
// JavaScript, or with reduced motion requested, the pages stay complete and static.
(() => {
  const root = document.documentElement;
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const finePointer = matchMedia('(hover: hover) and (pointer: fine)').matches;
  root.classList.add('js');

  // Mobile menu.
  const toggle = document.querySelector('.menu-toggle');
  const links = document.querySelector('.nav .links');
  if (toggle && links) {
    const close = () => { toggle.setAttribute('aria-expanded', 'false'); links.classList.remove('open'); };
    toggle.addEventListener('click', () => {
      const open = toggle.getAttribute('aria-expanded') !== 'true';
      toggle.setAttribute('aria-expanded', String(open));
      links.classList.toggle('open', open);
    });
    links.addEventListener('click', (e) => { if (e.target.closest('a')) close(); });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
  }

  // Reading progress and a denser nav once the page scrolls.
  const bar = document.querySelector('.progress');
  const nav = document.querySelector('.nav');
  let ticking = false;
  const onScroll = () => {
    ticking = false;
    const max = root.scrollHeight - innerHeight;
    if (bar) bar.style.transform = `scaleX(${max > 0 ? scrollY / max : 0})`;
    if (nav) nav.classList.toggle('scrolled', scrollY > 8);
  };
  addEventListener('scroll', () => { if (!ticking) { ticking = true; requestAnimationFrame(onScroll); } }, { passive: true });
  onScroll();

  // Reveal on scroll, staggered inside each group.
  const reveal = document.querySelectorAll('[data-reveal]');
  if (still || !('IntersectionObserver' in window)) {
    reveal.forEach((el) => el.classList.add('in'));
  } else {
    document.querySelectorAll('[data-stagger]').forEach((group) => {
      [...group.children].forEach((child, i) => {
        child.setAttribute('data-reveal', '');
        child.style.setProperty('--d', `${Math.min(i, 8) * 60}ms`);
      });
    });
    const io = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        entry.target.classList.add('in');
        io.unobserve(entry.target);
      }
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.12 });
    document.querySelectorAll('[data-reveal]').forEach((el) => io.observe(el));
  }

  // Animated numbers: <b data-countup="1234">.
  const countUp = (el) => {
    const target = Number(el.dataset.countup);
    if (!Number.isFinite(target)) return;
    if (still) { el.textContent = target.toLocaleString('es-ES'); return; }
    const start = performance.now();
    const step = (now) => {
      const t = Math.min(1, (now - start) / 1100);
      el.textContent = Math.round(target * (1 - Math.pow(1 - t, 3))).toLocaleString('es-ES');
      if (t < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  };
  window.nova5CountUp = countUp;
  if ('IntersectionObserver' in window) {
    const io = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        countUp(entry.target);
        io.unobserve(entry.target);
      }
    });
    document.querySelectorAll('[data-countup]').forEach((el) => io.observe(el));
  }

  if (still || !finePointer) return;

  // Spotlight that follows the pointer on cards.
  document.addEventListener('pointermove', (e) => {
    const card = e.target.closest('.card, .spot');
    if (!card) return;
    const r = card.getBoundingClientRect();
    card.style.setProperty('--mx', `${e.clientX - r.left}px`);
    card.style.setProperty('--my', `${e.clientY - r.top}px`);
  }, { passive: true });

  // Gentle 3D tilt on showcase frames.
  document.querySelectorAll('[data-tilt]').forEach((el) => {
    el.addEventListener('pointermove', (e) => {
      const r = el.getBoundingClientRect();
      const x = (e.clientX - r.left) / r.width - 0.5;
      const y = (e.clientY - r.top) / r.height - 0.5;
      el.style.transform = `perspective(1400px) rotateY(${x * 10}deg) rotateX(${-y * 8}deg)`;
    });
    el.addEventListener('pointerleave', () => { el.style.transform = ''; });
  });
})();
