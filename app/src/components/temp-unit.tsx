// °C / °F: it follows the locale and is changed in Settings. The unit's
// screen follows it (see setTempUnit).

import { setTempUnit } from '@/ble/session';
import type { TempUnit } from '@/ui/units';

import { Segmented } from './controls';

export function TempUnitPicker({ value, onChange }: { value: TempUnit; onChange?: (v: TempUnit) => void }) {
  return (
    <Segmented<TempUnit>
      value={value}
      onChange={onChange ?? ((v) => void setTempUnit(v).catch(() => {}))}
      options={[
        { value: 'C', label: 'Celsius (°C)' },
        { value: 'F', label: 'Fahrenheit (°F)' },
      ]}
    />
  );
}
