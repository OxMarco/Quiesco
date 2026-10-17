import { batteryDays, batteryLabel, intervalChoices } from '@/ui/battery';

describe('battery estimate', () => {
  test('longer intervals last longer', () => {
    expect(batteryDays(60)).toBeLessThan(batteryDays(300));
    expect(batteryDays(300)).toBeLessThan(batteryDays(600));
  });

  test('labels round down', () => {
    expect(batteryLabel(60)).toBe('~3 days');
    expect(batteryLabel(300)).toBe('~2 weeks');
    expect(batteryLabel(600)).toBe('~1 month');
  });

  test('Settings offers 1, 5 and 10 min, keeping a unit already on 30', () => {
    expect(intervalChoices(undefined)).toEqual([60, 300, 600]);
    expect(intervalChoices(300)).toEqual([60, 300, 600]);
    expect(intervalChoices(1800)).toEqual([60, 300, 600, 1800]);
  });
});
