package app.aemu

import android.app.Application
import app.aemu.core.CrashReport
import app.aemu.core.Keeper

class AemuApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Keeper.ensureChannel(this)
        // a crash inside the app used to leave nothing behind: the window closed and the user was back on the
        // library with no idea why
        CrashReport.install(this)
    }
}