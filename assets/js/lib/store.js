// Browser storage that never throws: private windows, blocked site data and quota errors all read
// as "nothing stored".

function area(name) {
  try {
    const s = window[name];
    const probe = "__scacelith__";
    s.setItem(probe, "1");
    s.removeItem(probe);
    return s;
  } catch {
    return null;
  }
}

const areas = { local: area("localStorage"), session: area("sessionStorage") };

export function get(key, where = "local") {
  try {
    const raw = areas[where]?.getItem(key);
    return raw === null || raw === undefined ? null : JSON.parse(raw);
  } catch {
    return null;
  }
}

export function set(key, value, where = "local") {
  try {
    areas[where]?.setItem(key, JSON.stringify(value));
    return Boolean(areas[where]);
  } catch {
    return false;
  }
}

export function remove(key, where = "local") {
  try {
    areas[where]?.removeItem(key);
  } catch {
    /* nothing to do */
  }
}
