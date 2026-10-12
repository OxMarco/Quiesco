// With more than one unit on the phone, the screens name the unit they show
// and open the switcher from that name. With one, nothing changes.

import { router } from 'expo-router';
import { Pressable, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';

import { useSession } from '@/ble/session';
import type * as db from '@/data/db';
import { useTheme } from '@/theme';

import { T, Title } from './ui';

function Chevron({ size }: { size: number }) {
  const t = useTheme();
  return (
    <Svg width={size} height={size} viewBox="0 0 20 20">
      <Path d="M5 8l5 5 5-5" fill="none" stroke={t.textMuted} strokeWidth={2} strokeLinecap="round" strokeLinejoin="round" />
    </Svg>
  );
}

const openSwitcher = () => router.push('/switch-unit');

/** The screen title: the shown unit's name with a switcher once there are several, else `children`. */
export function UnitTitle({ unit, detail, children }: { unit: db.Unit | null; detail?: React.ReactNode; children: string }) {
  const several = useSession((s) => s.units.length > 1);
  if (!several || !unit) return <Title detail={detail}>{children}</Title>;
  return (
    <View className="flex-row items-end justify-between pt-3 pb-1 gap-3">
      <Pressable
        onPress={openSwitcher}
        accessibilityRole="button"
        accessibilityLabel={`${unit.name}. Switch unit`}
        hitSlop={8}
        className="flex-row items-center gap-1.5 shrink active:opacity-60">
        <T className="font-wordmark text-[30px] leading-[38px] shrink" numberOfLines={1}>
          {unit.name}
        </T>
        <Chevron size={20} />
      </Pressable>
      {detail}
    </View>
  );
}

/** A small switcher for screens whose title names the screen, not the unit. */
export function UnitChip({ unit }: { unit: db.Unit | null }) {
  const several = useSession((s) => s.units.length > 1);
  if (!several || !unit) return null;
  return (
    <Pressable
      onPress={openSwitcher}
      accessibilityRole="button"
      accessibilityLabel={`Showing ${unit.name}. Switch unit`}
      hitSlop={6}
      className="flex-row items-center gap-1 h-9 px-3 rounded-full bg-raised shrink active:opacity-60">
      <T className="font-body-semi text-sm shrink" numberOfLines={1}>
        {unit.name}
      </T>
      <Chevron size={16} />
    </Pressable>
  );
}
