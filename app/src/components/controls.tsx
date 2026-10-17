import { Pressable, View } from 'react-native';

import { T } from './ui';

export function Segmented<V extends string | number>({
  options,
  value,
  onChange,
  disabled,
  label,
}: {
  options: { value: V; label: string }[];
  value: V | null;
  onChange: (v: V) => void;
  disabled?: boolean;
  /** Read out for the group, when the visible label is not next to it. */
  label?: string;
}) {
  return (
    <View
      className={`flex-row bg-raised rounded-full p-1 ${disabled ? 'opacity-40' : ''}`}
      accessibilityRole="radiogroup"
      accessibilityLabel={label}>
      {options.map((o) => {
        const on = o.value === value;
        return (
          <Pressable
            key={String(o.value)}
            disabled={disabled}
            onPress={() => onChange(o.value)}
            accessibilityRole="radio"
            accessibilityState={{ checked: on }}
            className={`flex-1 h-9 rounded-full items-center justify-center ${on ? 'bg-surface' : ''}`}>
            {/* A fixed-height capsule: capped and shrunk to fit, never clipped. */}
            <T
              className={`text-[13px] ${on ? 'font-body-semi' : 'text-muted'}`}
              maxFontSizeMultiplier={1.4}
              numberOfLines={1}
              adjustsFontSizeToFit
              minimumFontScale={0.7}>
              {o.label}
            </T>
          </Pressable>
        );
      })}
    </View>
  );
}

export function Stepper({
  label,
  value,
  step,
  min,
  max,
  unit,
  onChange,
  disabled,
}: {
  label: string;
  value: number;
  step: number;
  min: number;
  max: number;
  unit: string;
  onChange: (v: number) => void;
  disabled?: boolean;
}) {
  const clamp = (v: number) => Math.min(max, Math.max(min, Math.round(v / step) * step));
  // Offsets (a range through zero) show their sign; an absolute value like a CO₂ reference does not.
  const sign = min >= 0 ? '' : value > 0 ? '+' : value < 0 ? '−' : '';
  const text = `${sign}${Math.abs(value).toFixed(step < 1 ? 1 : 0)} ${unit}`;
  return (
    <View className="flex-row items-center justify-between py-2.5">
      <T className="text-[15px] shrink">{label}</T>
      <View className={`flex-row items-center gap-2 shrink-0 ${disabled ? 'opacity-40' : ''}`}>
        <StepButton label="−" a11y={`Decrease ${label}`} disabled={disabled || value <= min} onPress={() => onChange(clamp(value - step))} />
        {/* A minimum, not a fixed width: larger text sizes must not wrap "420 ppm". */}
        <T className="font-number-medium text-[15px] min-w-[68px] text-center" numberOfLines={1}>
          {text}
        </T>
        <StepButton label="+" a11y={`Increase ${label}`} disabled={disabled || value >= max} onPress={() => onChange(clamp(value + step))} />
      </View>
    </View>
  );
}

function StepButton({ label, a11y, onPress, disabled }: { label: string; a11y: string; onPress: () => void; disabled?: boolean }) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      accessibilityRole="button"
      accessibilityLabel={a11y}
      className={`w-9 h-9 rounded-full bg-raised items-center justify-center active:opacity-60 ${disabled ? 'opacity-40' : ''}`}>
      <T className="text-lg leading-6">{label}</T>
    </Pressable>
  );
}
