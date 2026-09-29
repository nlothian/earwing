// Save text as a file from the browser: a Blob URL and a temporary <a download>.
// Works over plain http (http://earwing.local) too; nothing leaves the browser.

// "earwing-transcript-2026-09-29-1432.txt": local time, sortable, no colons.
export function exportName(kind, ext, when = new Date()) {
  const p = (n) => String(n).padStart(2, '0');
  const stamp = `${when.getFullYear()}-${p(when.getMonth() + 1)}-${p(when.getDate())}-${p(when.getHours())}${p(when.getMinutes())}`;
  return `earwing-${kind}-${stamp}.${ext}`;
}

export function downloadText(filename, text, type = 'text/plain') {
  const url = URL.createObjectURL(new Blob([text.endsWith('\n') ? text : `${text}\n`], { type: `${type};charset=utf-8` }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  a.remove();
  // Some browsers read the URL after click() returns.
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
