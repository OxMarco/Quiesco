// iOS: the WidgetKit extension (targets/widget) reads the snapshot as JSON
// from the shared App Group, then the timeline is reloaded to redraw it.

import { ExtensionStorage } from '@bacons/apple-targets';

import type { WidgetSnapshot } from './snapshot';

/** Must match ios.entitlements in app.json and targets/widget/expo-target.config.js. */
export const APP_GROUP = 'group.rest.quiesco.app';
const storage = new ExtensionStorage(APP_GROUP);

export async function writeSnapshot(snapshot: WidgetSnapshot): Promise<void> {
  storage.set('snapshot', JSON.stringify(snapshot));
  ExtensionStorage.reloadWidget();
}
