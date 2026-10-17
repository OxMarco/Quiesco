// The periodic background run, on both platforms: Android schedules it with
// WorkManager (every 15 minutes at best, later in Doze), iOS with
// BGTaskScheduler (when the system chooses, often overnight on charge). Each
// run syncs the shown unit, which sets its clock, then refreshes the widgets.
//
// defineTask must run at module scope before the app renders, so the entry
// (index.ts) imports this file first: a run may start the app with no screen.

import * as BackgroundTask from 'expo-background-task';
import * as TaskManager from 'expo-task-manager';

import { backgroundSync } from '@/ble/session';
import { trace } from '@/debug/trace';
import { updateWidgets } from '@/widgets';

export const SYNC_TASK = 'quiesco-background-sync';

TaskManager.defineTask(SYNC_TASK, async () => {
  try {
    await backgroundSync('periodic');
    await updateWidgets();
    return BackgroundTask.BackgroundTaskResult.Success;
  } catch (e) {
    trace('background task failed', e);
    return BackgroundTask.BackgroundTaskResult.Failed;
  }
});

/** Once per launch; registering again only updates the interval. */
export async function registerBackgroundSync(): Promise<void> {
  if ((await BackgroundTask.getStatusAsync()) !== BackgroundTask.BackgroundTaskStatus.Available) {
    trace('background tasks restricted');
    return;
  }
  await BackgroundTask.registerTaskAsync(SYNC_TASK, { minimumInterval: 15 });
}
