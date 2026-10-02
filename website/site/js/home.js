// Landing page: shows which release each channel and each device is at, read
// from firmware/index.json.

const SITE_ROOT = new URL('../', import.meta.url);
const TITLES = { stable: 'Stable', dev: 'Dev' };

async function start() {
  const response = await fetch(new URL('firmware/index.json', SITE_ROOT), { cache: 'no-store' });
  if (!response.ok) throw new Error(`firmware/index.json: HTTP ${response.status}`);
  const index = await response.json();

  const releases = Object.entries(TITLES)
    .map(([id, title]) => ({ title, release: index.channels[id]?.release }))
    .filter((channel) => channel.release);

  const versions = document.getElementById('versions');
  for (const { title, release } of releases) {
    if (versions.childNodes.length) versions.append(' · ');
    const tag = document.createElement('b');
    tag.textContent = release.tag;
    versions.append(`${title} `, tag);
  }

  for (const card of document.querySelectorAll('[data-board]')) {
    const slug = card.dataset.board;
    const where = releases.filter(({ release }) => release.boards[slug]).map(({ title, release }) => `${title} ${release.tag}`);
    card.querySelector('.tagline').textContent = where.length ? where.join(' · ') : 'No published build yet';
  }
}

start();
