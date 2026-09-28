// °F for visitors whose clock is set to a time zone in the US or one of the few
// other places that use Fahrenheit; °C everywhere else. The time zone comes from
// the browser, so nothing about the visitor leaves their device.

const F_ZONES = new Set([
  // United States
  'America/New_York', 'America/Detroit', 'America/Chicago', 'America/Menominee',
  'America/Denver', 'America/Boise', 'America/Phoenix', 'America/Los_Angeles',
  'America/Anchorage', 'America/Juneau', 'America/Sitka', 'America/Metlakatla',
  'America/Yakutat', 'America/Nome', 'America/Adak', 'Pacific/Honolulu',
  'America/Indianapolis', 'America/Louisville', 'America/Fort_Wayne', 'America/Knox_IN',
  'Navajo', 'Pacific/Johnston',
  // US territories
  'America/Puerto_Rico', 'America/St_Thomas', 'Pacific/Guam', 'Pacific/Saipan',
  'Pacific/Pago_Pago', 'Pacific/Samoa', 'Pacific/Midway', 'Pacific/Wake',
  // Other countries that use °F
  'America/Nassau', 'America/Belize', 'America/Cayman', 'Africa/Monrovia',
  'Pacific/Palau', 'Pacific/Majuro', 'Pacific/Kwajalein', 'Pacific/Chuuk',
  'Pacific/Truk', 'Pacific/Pohnpei', 'Pacific/Ponape', 'Pacific/Kosrae',
]);
const F_PREFIXES = ['US/', 'America/Indiana/', 'America/Kentucky/', 'America/North_Dakota/'];

export function usesFahrenheit(): boolean {
  try {
    const zone = Intl.DateTimeFormat().resolvedOptions().timeZone ?? '';
    return F_ZONES.has(zone) || F_PREFIXES.some((p) => zone.startsWith(p));
  } catch {
    return false;
  }
}

export const toF = (c: number) => (c * 9) / 5 + 32;

/** A temperature in the chosen unit, rounded like the panel. */
export const formatTemp = (c: number, fahrenheit: boolean) => Math.round(fahrenheit ? toF(c) : c);

/**
 * Rewrites every server-rendered temperature (`data-c` holds the °C value,
 * `data-deg` marks a unit symbol) into °F.
 */
export function applyFahrenheit(root: ParentNode = document) {
  root.querySelectorAll<HTMLElement>('[data-c]').forEach((el) => {
    el.textContent = String(formatTemp(Number(el.dataset.c), true));
  });
  root.querySelectorAll<HTMLElement>('[data-deg]').forEach((el) => {
    el.textContent = el.textContent!.replace('°C', '°F');
  });
}
