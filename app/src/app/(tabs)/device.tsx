// Device: the shown unit's own settings, one tap from the tab bar. Other
// units are reached from the title's switcher or Settings' list.

import { useShownUnit } from '@/ble/session';
import { UnitSettings } from '@/components/unit-settings';
import { UnitTitle } from '@/components/unit-title';
import { Screen } from '@/components/ui';

export default function DeviceScreen() {
  const unit = useShownUnit();
  return (
    <Screen tabBar>
      <UnitTitle unit={unit}>Device</UnitTitle>
      {/* Keyed so a switch drops the last unit's errors and pending state. */}
      {unit && <UnitSettings key={unit.serial} unit={unit} />}
    </Screen>
  );
}
