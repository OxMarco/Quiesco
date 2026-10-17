// Step two of adding a unit: its name. Saved on the unit, which advertises it
// over Bluetooth, so it is the same in the app and in any Bluetooth list.

import { router, useLocalSearchParams } from 'expo-router';
import { useMemo, useState } from 'react';
import { ScrollView, TextInput, View } from 'react-native';

import { describeError, link, setName } from '@/ble/link';
import { reloadUnits, useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import { Button, Sheet, SheetHeader, T } from '@/components/ui';
import { DEVICE_NAME_MAX_BYTES, deviceNameProblem } from '@/protocol/codec';
import { useTheme } from '@/theme';

export default function NameUnitScreen() {
  const t = useTheme();
  const { serial } = useLocalSearchParams<{ serial: string }>();
  const unit = useSession((s) => s.units.find((u) => u.serial === serial) ?? null);
  // Select the stored array and derive from it: a selector that builds a new
  // array every call never compares equal, and re-renders without end.
  const units = useSession((s) => s.units);
  const others = useMemo(() => units.filter((u) => u.serial !== serial).map((u) => u.name), [units, serial]);
  const connected = useStore(link, (s) => s.phase === 'ready' && s.serial === serial);
  // null until the user types: the unit's name shows even if it loads after this opens.
  const [edited, setDraft] = useState<string | null>(null);
  const draft = edited ?? unit?.name ?? '';
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const trimmed = draft.trim();
  const changed = trimmed !== unit?.name;
  const problem = changed ? deviceNameProblem(trimmed) : null;
  const taken = others.includes(trimmed) ? 'Another unit on this phone has this name.' : null;

  const next = () => router.replace({ pathname: '/sleep-window', params: { serial, setup: '1' } });

  const save = () => {
    if (!changed) return next();
    if (!connected) {
      setError('The unit disconnected. Bring it closer, or rename it later in Settings.');
      return;
    }
    setBusy(true);
    setError(null);
    setName(trimmed)
      .then(reloadUnits)
      .then(next)
      .catch((e) => setError(describeError(e)))
      .finally(() => setBusy(false));
  };

  return (
    <Sheet>
      <ScrollView contentContainerClassName="px-4 pt-6 pb-6 gap-4" keyboardShouldPersistTaps="handled">
        <SheetHeader eyebrow="Step 2 of 3" title="Name it" action="Skip" onAction={next} />
        <View className="gap-2">
          <T className="font-body-semi text-sm px-1">Name</T>
          <TextInput
            value={draft}
            onChangeText={setDraft}
            autoFocus
            maxLength={DEVICE_NAME_MAX_BYTES}
            returnKeyType="next"
            onSubmitEditing={() => !problem && save()}
            selectionColor={t.primary}
            accessibilityLabel="Name"
            className="h-12 rounded-xl bg-surface px-3.5 font-body text-[17px] text-ink"
          />
          <T className={`text-[13px] leading-[18px] px-1 ${problem || error ? 'text-bad' : 'text-muted'}`}>
            {problem ?? error ?? taken ?? `Up to ${DEVICE_NAME_MAX_BYTES} characters. You can change it later.`}
          </T>
        </View>
        {/* Right under the field, not at the bottom: the field focuses at once
            and the keyboard would cover a bottom button. */}
        <Button title="Next" busy={busy} disabled={!!problem} onPress={save} />
      </ScrollView>
    </Sheet>
  );
}
