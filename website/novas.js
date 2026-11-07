// NovaS page: the console preview (NovaS's home screen driven by keyboard, mouse, touch or the drawn controllers),
// the games shelf, the FPS tuner and the download links of the latest release.
(() => {
	'use strict';
	const REPO = 'nova5emu/novas';
	const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;

	// Real Switch 2 games. state: probado (played by us in NovaS), jugable / medias / arreglo (what the community reports
	// with emulators of the same base, not checked in NovaS yet), sin (no reports).
	const GAMES = [
		{ id: 'mario-kart-world', name: 'Mario Kart World', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros. Lleva además arreglos propios para el modo foto y varios efectos.' },
		{ id: 'zelda-ocarina-of-time', name: 'The Legend of Zelda: Ocarina of Time', state: 'probado', note: 'Probado en NovaS: 60 FPS reales en zonas tranquilas y 49 a 57 en el pueblo Kokiri. Con FSR3 se ven hasta 120 en pantalla.' },
		{ id: 'pokemon-pokopia', name: 'Pokémon Pokopia', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'pokemon-legends-z-a', name: 'Pokémon Legends: Z-A', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'donkey-kong-bananza', name: 'Donkey Kong Bananza', state: 'medias', note: 'La comunidad lo reporta jugable a medias en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros. NovaS trae dos mejoras de rendimiento propias para este juego, apagadas mientras las probamos.' },
		{ id: 'nintendo-switch-sports-resort', name: 'Nintendo Switch Sports Resort', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'splatoon-raiders', name: 'Splatoon Raiders', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'kirby-air-riders', name: 'Kirby Air Riders', state: 'medias', note: 'La comunidad lo reporta jugable a medias en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros. NovaS lleva una corrección de texturas para este juego.' },
		{ id: 'super-mario-bros-wonder', name: 'Super Mario Bros. Wonder: Nintendo Switch 2 Edition', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'mario-tennis-fever', name: 'Mario Tennis Fever', state: 'arreglo', note: 'La comunidad lo reporta todavía en arreglo (no se puede jugar entero) en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'super-mario-galaxy-2', name: 'Super Mario Galaxy 2', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'fire-emblem-fortunes-weave', name: "Fire Emblem: Fortune's Weave", state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'yoshi-and-the-mysterious-book', name: 'Yoshi and the Mysterious Book', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros. NovaS lleva un arreglo de sombreado para este juego.' },
		{ id: 'pragmata', name: 'Pragmata', state: 'arreglo', note: 'La comunidad lo reporta todavía en arreglo (no se puede jugar entero) en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'hyrule-warriors-age-of-imprisonment', name: 'Hyrule Warriors: Age of Imprisonment', state: 'medias', note: 'La comunidad lo reporta jugable a medias en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'cyberpunk-2077', name: 'Cyberpunk 2077: Ultimate Edition', state: 'medias', note: 'La comunidad lo reporta jugable a medias en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'deltarune', name: 'Deltarune', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'fast-fusion', name: 'Fast Fusion', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'red-dead-redemption', name: 'Red Dead Redemption', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'resident-evil-4', name: 'Resident Evil 4', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'metroid-prime-4-beyond', name: 'Metroid Prime 4: Beyond', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'star-fox', name: 'Star Fox', state: 'jugable', note: 'La comunidad lo reporta jugable a 60 FPS en emuladores con la misma base que NovaS. Aún no lo hemos comprobado nosotros.' },
		{ id: 'drag-x-drive', name: 'Drag x Drive', state: 'sin', note: 'Todavía no hay informes de este juego.' },
	];
	const STATE_LABEL = { probado: 'Probado en NovaS', jugable: 'Jugable', medias: 'Jugable a medias', arreglo: 'En arreglo', sin: 'Sin datos' };
	const cover = g => `assets/novas/juegos/${g.id}.webp`;
	const $ = (sel, root = document) => root.querySelector(sel);
	const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

	// ---- Sound: a short synthesized tick, off until the visitor turns it on ----
	let audio = null, soundOn = false;
	function tick(freq = 1250, length = .035) {
		if (!soundOn) return;
		audio ??= new AudioContext();
		const osc = audio.createOscillator(), gain = audio.createGain();
		osc.type = 'square';
		osc.frequency.value = freq;
		gain.gain.setValueAtTime(.05, audio.currentTime);
		gain.gain.exponentialRampToValueAtTime(.0001, audio.currentTime + length);
		osc.connect(gain).connect(audio.destination);
		osc.start();
		osc.stop(audio.currentTime + length);
	}
	const soundBtn = $('[data-sound]');
	soundBtn?.addEventListener('click', () => {
		soundOn = !soundOn;
		soundBtn.setAttribute('aria-pressed', String(soundOn));
		soundBtn.textContent = soundOn ? 'Sonido: sí' : 'Sonido: no';
		tick(1600);
	});

	// ---- Console preview ----
	const screen = $('.screen');
	const row = $('[data-row]');
	const name = $('[data-name]');
	const detail = $('[data-detail]');
	const buttons = $$('[data-buttons] button');
	const boot = $('[data-boot]');
	const toastEl = $('[data-toast]');
	let index = Math.max(0, GAMES.findIndex(g => g.state === 'probado')), onButtons = false, button = 0, booted = false, toastTimer;

	GAMES.forEach((g, i) => {
		const li = document.createElement('li');
		li.innerHTML = `<button class="tile" type="button" tabindex="-1" aria-label="${g.name}"><img src="${cover(g)}" alt="" width="320" height="320" loading="${i < 6 ? 'eager' : 'lazy'}"></button>`;
		li.firstChild.addEventListener('click', () => {
			if (onButtons || index !== i) { onButtons = false; index = i; refresh(); tick(); }
			else open();
		});
		row.append(li);
	});
	const add = document.createElement('li');
	add.innerHTML = '<button class="tile tile-add" type="button" tabindex="-1" aria-label="Añadir carpeta">+</button>';
	add.firstChild.addEventListener('click', () => { onButtons = false; index = GAMES.length; refresh(); toast('Aquí eliges la carpeta con tus juegos'); });
	row.append(add);

	buttons.forEach((b, i) => b.addEventListener('click', () => { onButtons = true; button = i; refresh(); press(b.dataset.action); }));

	function toast(text) {
		toastEl.textContent = text;
		toastEl.classList.add('show');
		clearTimeout(toastTimer);
		toastTimer = setTimeout(() => toastEl.classList.remove('show'), 2600);
	}

	function press(action) {
		tick(900, .06);
		const messages = {
			carpeta: 'Eliges la carpeta con tus .nsp, .xci o .nup',
			claves: 'Eliges el prod.keys de tu consola',
			firmware: 'Eliges el firmware de tu consola (.zip o .nca)',
			ajustes: 'Resolución, FSR3, FPS de Zelda y más',
		};
		if (action === 'descargar') { location.href = $('[data-download]').href; return; }
		toast(messages[action]);
	}

	function refresh() {
		const tiles = $$('.tile', row);
		tiles.forEach((t, i) => t.classList.toggle('on', !onButtons && i === index));
		buttons.forEach((b, i) => b.classList.toggle('on', onButtons && i === button));
		const first = tiles[0];
		if (first) {
			const step = first.getBoundingClientRect().width + parseFloat(getComputedStyle(row).columnGap || 0);
			row.style.transform = `translateX(${-Math.max(0, index - 3) * step}px)`;
		}
		if (onButtons) {
			name.textContent = buttons[button].parentElement.querySelector('span').textContent;
			detail.textContent = '';
		} else if (index < GAMES.length) {
			const g = GAMES[index];
			name.textContent = g.name;
			detail.textContent = g.state === 'probado' ? 'Probado en NovaS: 60 FPS reales, hasta 120 con FSR3' : g.state === 'jugable' ? 'Jugable a 60 FPS, según la comunidad' : STATE_LABEL[g.state];
		} else {
			name.textContent = 'Añadir carpeta';
			detail.textContent = 'Elige dónde guardas tus juegos de Switch 2';
		}
	}

	function open() {
		if (onButtons) { press(buttons[button].dataset.action); return; }
		if (index >= GAMES.length) { toast('Aquí eliges la carpeta con tus juegos'); return; }
		const g = GAMES[index];
		tick(700, .09);
		$('[data-boot-cover]').src = cover(g);
		$('[data-boot-name]').textContent = g.name;
		const state = $('[data-boot-state]');
		state.textContent = STATE_LABEL[g.state];
		state.className = `boot-state ${g.state}`;
		$('[data-boot-note]').textContent = g.note;
		screen.classList.add('booting');
		const show = () => { boot.hidden = false; boot.classList.add('show'); booted = true; };
		reduced ? show() : setTimeout(show, 260);
	}

	function back() {
		if (!booted) { if (onButtons) { onButtons = false; refresh(); } return; }
		tick(500, .06);
		boot.hidden = true;
		boot.classList.remove('show');
		screen.classList.remove('booting');
		booted = false;
	}

	function input(key) {
		if (booted) { if (key === 'b' || key === 'a') back(); return; }
		switch (key) {
			case 'left':  move(-1); break;
			case 'right': move(1); break;
			case 'down':  if (!onButtons) { onButtons = true; tick(); refresh(); } break;
			case 'up':    if (onButtons) { onButtons = false; tick(); refresh(); } break;
			case 'a':     open(); break;
			case 'b':     back(); break;
			case 'y':     onButtons = true; button = 3; refresh(); press('ajustes'); break;
			case 'x':     toast('X no hace nada aquí'); break;
		}
	}

	function move(step) {
		if (onButtons) button = Math.min(buttons.length - 1, Math.max(0, button + step));
		else index = Math.min(GAMES.length, Math.max(0, index + step));
		tick();
		refresh();
	}

	screen.addEventListener('keydown', e => {
		const keys = { ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down', Enter: 'a', ' ': 'a', Escape: 'b', Backspace: 'b', y: 'y', Y: 'y' };
		const key = keys[e.key];
		if (!key) return;
		e.preventDefault();
		input(key);
	});
	screen.addEventListener('click', e => { if (e.target === screen || e.target.closest('.boot')) { if (booted) back(); } });

	// Swipe on touch screens.
	let touchX = null;
	screen.addEventListener('touchstart', e => { touchX = e.touches[0].clientX; }, { passive: true });
	screen.addEventListener('touchend', e => {
		if (touchX === null) return;
		const dx = e.changedTouches[0].clientX - touchX;
		if (Math.abs(dx) > 40) move(dx < 0 ? 2 : -2);
		touchX = null;
	});

	// The drawn controllers: every button answers like a real one.
	$$('[data-pad]').forEach(b => b.addEventListener('click', () => {
		b.classList.add('hit');
		setTimeout(() => b.classList.remove('hit'), 120);
		input(b.dataset.pad);
		screen.focus({ preventScroll: true });
	}));
	addEventListener('resize', refresh);

	// The clock at the top of the screen.
	const clock = $('[data-clock]');
	const tickClock = () => { clock.textContent = new Date().toLocaleTimeString('es', { hour: '2-digit', minute: '2-digit' }); };
	tickClock();
	setInterval(tickClock, 15000);
	refresh();

	// ---- Games shelf ----
	const shelf = $('[data-shelf]');
	GAMES.forEach(g => {
		const li = document.createElement('li');
		li.dataset.state = g.state;
		li.innerHTML = `<button class="game" type="button" aria-expanded="false">
			<span class="game-cover"><img src="${cover(g)}" alt="" width="320" height="320" loading="lazy"></span>
			<span class="game-name">${g.name}</span>
			<span class="state ${g.state}">${STATE_LABEL[g.state]}</span>
			<span class="game-note">${g.note}</span></button>`;
		const btn = li.firstElementChild;
		btn.addEventListener('click', () => btn.setAttribute('aria-expanded', String(btn.getAttribute('aria-expanded') !== 'true')));
		shelf.append(li);
	});
	$$('[data-filter]').forEach(chip => chip.addEventListener('click', () => {
		$$('[data-filter]').forEach(c => { c.classList.toggle('on', c === chip); c.setAttribute('aria-pressed', String(c === chip)); });
		const f = chip.dataset.filter;
		$$('li', shelf).forEach(li => { li.hidden = f !== 'todos' && !(f === 'jugable' ? ['probado', 'jugable'] : [f]).includes(li.dataset.state); });
	}));

	// ---- FPS tuner: values measured with NovaS's Zelda benchmark (see the 07/11 night notes) ----
	const DATA = {
		'1x-no': { tienda: [60, '60'], camino: [52, '52'], pueblo: [55.5, '54–57'], note: 'FPS reales del juego, a 1440p (la resolución nativa del monitor de la prueba).' },
		'4k-no': { tienda: [60, '60'], camino: [43, '43'], pueblo: [50, '49–51'], note: 'FPS reales del juego, dibujando a 4K. La tarjeta gráfica trabaja más y el pueblo baja unos 6 FPS.' },
		'1x-si': { tienda: [120, '120'], camino: [110, '100–120'], pueblo: [88, '62–115'], note: 'Imágenes en pantalla con FSR3 y "60 dinámico". El juego calcula la mitad; FSR3 genera las intermedias.' },
		'4k-si': null,
	};
	const meters = $('[data-meters]');
	const note = $('[data-tuner-note]');
	function tune() {
		const res = $('input[name=res]:checked').value, fg = $('input[name=fg]:checked').value;
		const d = DATA[`${res}-${fg}`];
		meters.classList.toggle('fg', fg === 'si');
		for (const place of ['tienda', 'camino', 'pueblo']) {
			const b = $(`[data-m=${place}]`, meters);
			const bar = b.parentElement.querySelector('.meter-bar i');
			b.textContent = d ? d[place][1] : '—';
			bar.style.setProperty('--w', d ? `${Math.min(100, d[place][0] / 120 * 100)}%` : '0%');
		}
		note.textContent = d ? d.note : 'Esta combinación todavía no la hemos medido.';
	}
	$$('.tuner input').forEach(i => i.addEventListener('change', tune));
	tune();

	// ---- Video: loads only when asked ----
	const video = $('[data-video]');
	const play = $('[data-play]');
	play?.addEventListener('click', () => {
		video.preload = 'auto';
		video.controls = true;
		video.play().then(() => video.closest('.tv').classList.add('playing')).catch(() => {});
	});

	// ---- Download counter: every file of every release, as GitHub counts them ----
	fetch(`https://api.github.com/repos/${REPO}/releases?per_page=100`, { headers: { Accept: 'application/vnd.github+json' } })
		.then(r => (r.ok ? r.json() : Promise.reject(r.status)))
		.then(releases => {
			const total = releases.flatMap(r => r.assets || []).reduce((sum, a) => sum + (a.download_count || 0), 0);
			if (!total) return;
			$$('[data-download-count]').forEach(b => { b.textContent = total.toLocaleString('es'); });
			$$('[data-downloads]').forEach(s => { s.hidden = false; });
		})
		.catch(() => {});

	// ---- Downloads: the latest release's files, version and size ----
	fetch(`https://api.github.com/repos/${REPO}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' } })
		.then(r => (r.ok ? r.json() : Promise.reject(r.status)))
		.then(rel => {
			const asset = n => (rel.assets || []).find(a => a.name === n);
			const setup = asset('NovaS-Setup.exe'), zip = asset('NovaS-Portable.zip');
			if (setup) $$('[data-download]').forEach(a => { a.href = setup.browser_download_url; });
			if (zip) $$('[data-portable]').forEach(a => { a.href = zip.browser_download_url; });
			const v = (rel.tag_name || '').replace(/^v/i, '');
			if (v) $$('[data-version]').forEach(s => { s.textContent = `Versión ${v}`; });
			if (setup) $$('[data-size]').forEach(s => { s.textContent = `${Math.round(setup.size / 1e6)} MB.`; });
		})
		.catch(() => {});
})();
