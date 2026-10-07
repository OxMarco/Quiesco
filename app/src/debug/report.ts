// A plain-text diagnostic report for support: the app and phone, the unit as
// the link last saw it, and the recent trace. It holds no readings and no key
// material, only identifiers (unit serial, sensor serial) support needs.

import * as Application from 'expo-application';
import Constants from 'expo-constants';
import * as Device from 'expo-device';
import { Platform } from 'react-native';

import type { LinkState } from '@/ble/link';
import type * as db from '@/data/db';
import { describeResetReason } from '@/protocol/codec';

import { traceLines } from './trace';

export const SUPPORT_EMAIL = 'info@impossiblelabs.xyz';

export function buildReport(link: LinkState, saved: db.Unit | null, now = new Date()): string {
  const out: string[] = [];
  const add = (label: string, value: unknown) => out.push(`${label}: ${value ?? '--'}`);

  out.push('Quiesco diagnostic report', now.toISOString(), '');
  out.push('## App');
  add('Version', `${Constants.expoConfig?.version ?? '--'} (${Application.nativeBuildVersion ?? '--'})`);
  add('Phone', `${Device.manufacturer ?? ''} ${Device.modelName ?? ''}`.trim() || null);
  add('System', `${Platform.OS} ${Device.osVersion ?? Platform.Version}`);
  add('Bluetooth link', link.error ? `${link.phase} (${link.error})` : link.phase);

  out.push('', '## Unit');
  if (saved) {
    add('Saved unit', `${saved.serial}, firmware ${saved.firmware ?? '--'}`);
    add('History synced', saved.lastSyncMs ? new Date(saved.lastSyncMs).toISOString() : 'never');
  }
  if (!link.serial && !link.info) {
    out.push(saved ? 'Not connected right now.' : 'No unit on this phone.');
  } else {
    add('Serial', link.serial);
    add('Firmware', link.firmware ?? link.info?.firmware);
    if (link.info) {
      add('Protocol', `${link.info.protocolVersion} (capabilities 0x${link.info.capabilities.toString(16)})`);
      add('Debug build', link.info.debugBuild ? 'yes' : 'no');
      add('Boot counter', link.info.bootCounter);
      add('CO₂ sensor serial', link.info.scd41Serial ? link.info.scd41Serial.toString(16) : null);
    }
    if (link.diagnostics) {
      add('Last reset', describeResetReason(link.diagnostics.resetReason));
      add('Uptime', formatUptime(link.diagnostics.uptimeSeconds));
      add('I²C bus stuck', link.diagnostics.i2cBusStuck ? 'yes' : 'no');
    }
    if (link.status) {
      const s = link.status;
      add('Clock synced', s.clockSynced ? 'yes' : 'no');
      add('Newest log record', s.newestSequence);
      add('Log erasing', s.logErasing ? 'yes' : 'no');
      add('Sensors present / valid / timed out', `0x${s.presentMask.toString(16)} / 0x${s.validMask.toString(16)} / 0x${s.timedOutMask.toString(16)}`);
      add('Sensor failures', s.failures.join(' '));
    }
    if (link.calibration) {
      add('CO₂ self-calibration', link.calibration.ascOff ? 'off' : 'on');
      add('Last CO₂ recalibration', link.calibration.lastFrcEpochS ? new Date(link.calibration.lastFrcEpochS * 1000).toISOString() : 'never');
    }
    if (link.sync) {
      add('Last sync', `${link.sync.state}, ${link.sync.received} records${link.sync.message ? ` (${link.sync.message})` : ''}`);
    }
  }

  const lines = traceLines();
  out.push('', `## Log (last ${lines.length} lines, UTC)`, ...lines);
  return out.join('\n');
}

function formatUptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min`;
}
