package com.claudemonitor

import com.claudemonitor.data.AppSettings
import com.claudemonitor.data.NotificationRules
import com.claudemonitor.data.Protocol.ClaudeState
import com.claudemonitor.data.TaskNotification
import com.claudemonitor.data.allows
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NotificationRulesTest {

    private fun rule(previous: ClaudeState?, next: ClaudeState, note: String? = null) =
        NotificationRules.forTransition(previous, next, note)

    @Test
    fun newRequestStartsWork() {
        assertEquals(TaskNotification.STARTED, rule(ClaudeState.IDLE, ClaudeState.WORKING))
        assertEquals(TaskNotification.STARTED, rule(ClaudeState.LIMITED, ClaudeState.WORKING))
    }

    @Test
    fun resumingAfterPermissionOrToolErrorIsNotANewStart() {
        assertNull(rule(ClaudeState.WAITING, ClaudeState.WORKING))
        assertNull(rule(ClaudeState.ERROR, ClaudeState.WORKING))
    }

    @Test
    fun finishedAndInterruptedReplies() {
        assertEquals(TaskNotification.FINISHED, rule(ClaudeState.WORKING, ClaudeState.IDLE))
        assertEquals(
            TaskNotification.INTERRUPTED,
            rule(ClaudeState.WORKING, ClaudeState.IDLE, NotificationRules.INTERRUPTED_NOTE),
        )
    }

    @Test
    fun limitIsReportedButItsReleaseIsSilent() {
        assertEquals(TaskNotification.LIMIT, rule(ClaudeState.WORKING, ClaudeState.LIMITED, "10:30pm"))
        assertNull(rule(ClaudeState.LIMITED, ClaudeState.IDLE))
    }

    @Test
    fun waitingForUserIsReported() {
        assertEquals(TaskNotification.WAITING, rule(ClaudeState.WORKING, ClaudeState.WAITING))
    }

    @Test
    fun unknownTaskOrSameStateIsSilent() {
        assertNull(rule(null, ClaudeState.WORKING))
        assertNull(rule(ClaudeState.WORKING, ClaudeState.WORKING))
        assertNull(rule(ClaudeState.STARTING, ClaudeState.IDLE))
    }

    @Test
    fun settingsFilterNotifications() {
        assertFalse("вимкнені сповіщення не показуються", AppSettings().allows(TaskNotification.FINISHED))

        val settings = AppSettings(notificationsEnabled = true, notifyStarted = false)
        assertFalse(settings.allows(TaskNotification.STARTED))
        assertTrue(settings.allows(TaskNotification.FINISHED))
        assertTrue(settings.allows(TaskNotification.INTERRUPTED))
        assertTrue(settings.allows(TaskNotification.LIMIT))
        assertTrue(settings.allows(TaskNotification.WAITING))
    }
}
