/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.nio.charset.Charset;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Hashtable;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.json.JSONException;
import org.json.JSONArray;
import org.json.JSONObject;
import org.mozilla.gecko.annotation.RobocopTarget;
import org.mozilla.gecko.GeckoProfileDirectories.NoMozillaDirectoryException;
import org.mozilla.gecko.GeckoProfileDirectories.NoSuchProfileException;
import org.mozilla.gecko.db.BrowserDB;
import org.mozilla.gecko.db.LocalBrowserDB;
import org.mozilla.gecko.db.StubBrowserDB;
import org.mozilla.gecko.distribution.Distribution;
import org.mozilla.gecko.mozglue.ContextUtils;
import org.mozilla.gecko.firstrun.FirstrunAnimationContainer;
import org.mozilla.gecko.preferences.DistroSharedPrefsImport;
import org.mozilla.gecko.util.INIParser;
import org.mozilla.gecko.util.INISection;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.SharedPreferences;
import android.support.annotation.WorkerThread;
import android.text.TextUtils;
import android.util.Log;

public final class GeckoProfile {
    private static final String LOGTAG = "GeckoProfile";

    // The path in the profile to the file containing the client ID.
    private static final String CLIENT_ID_FILE_PATH = "datareporting/state.json";
    private static final String FHR_CLIENT_ID_FILE_PATH = "healthreport/state.json";
    // In the client ID file, the attribute title in the JSON object containing the client ID value.
    private static final String CLIENT_ID_JSON_ATTR = "clientID";

    private static final String TIMES_PATH = "times.json";
    private static final String PROFILE_CREATION_DATE_JSON_ATTR = "created";

    // Only tests should need to do this.
    // We can default this to AppConstants.RELEASE_BUILD once we fix Bug 1069687.
    private static volatile boolean sAcceptDirectoryChanges = true;

    @RobocopTarget
    public static void enableDirectoryChanges() {
        Log.w(LOGTAG, "Directory changes should only be enabled for tests. And even then it's a bad idea.");
        sAcceptDirectoryChanges = true;
    }

    // Used to "lock" the guest profile, so that we'll always restart in it
    private static final String LOCK_FILE_NAME = ".active_lock";
    public static final String DEFAULT_PROFILE = "default";
    // Profile is using a custom directory outside of the Mozilla directory.
    public static final String CUSTOM_PROFILE = "";
    public static final String GUEST_PROFILE = "guest";

    private static final HashMap<String, GeckoProfile> sProfileCache = new HashMap<String, GeckoProfile>();
    private static String sDefaultProfileName;

    // Caches the guest profile dir.
    private static File sGuestDir;
    private static GeckoProfile sGuestProfile;
    private static boolean sShouldCheckForGuestProfile = true;

    private final String mName;
    private final File mMozillaDir;
    private final Context mApplicationContext;

    private final BrowserDB mDB;

    /**
     * Access to this member should be synchronized to avoid
     * races during creation -- particularly between getDir and GeckoView#init.
     *
     * Not final because this is lazily computed.
     */
    private File mProfileDir;

    // Caches whether or not a profile is "locked".
    // Only used by the guest profile to determine if it should be reused or
    // deleted on startup.
    // These are volatile for an incremental improvement in thread safety,
    // but this is not a complete solution for concurrency.
    private volatile LockState mLocked = LockState.UNDEFINED;
    private volatile boolean mInGuestMode;

    // Constants to cache whether or not a profile is "locked".
    private enum LockState {
        LOCKED,
        UNLOCKED,
        UNDEFINED
    };

    /**
     * Can return null.
     */
    public static GeckoProfile getFromArgs(final Context context, final String args) {
        if (args == null) {
            return null;
        }

        if (GuestSession.shouldUse(context, args)) {
            return GeckoProfile.getOrCreateGuestProfile(context);
        }

        String profileName = null;
        String profilePath = null;
        if (args.contains("-P")) {
            final Pattern p = Pattern.compile("(?:-P\\s*)(\\w*)(\\s*)");
            final Matcher m = p.matcher(args);
            if (m.find()) {
                profileName = m.group(1);
            }
        }

        if (args.contains("-profile")) {
            final Pattern p = Pattern.compile("(?:-profile\\s*)(\\S*)(\\s*)");
            final Matcher m = p.matcher(args);
            if (m.find()) {
                profilePath =  m.group(1);
            }
        }

        if (profileName == null && profilePath == null) {
            return null;
        }

        return GeckoProfile.get(context, profileName, profilePath);
    }

    public static GeckoProfile getDefaultProfile(Context context) {
        try {
            return get(context, getDefaultProfileName(context));

        } catch (final NoMozillaDirectoryException e) {
            // If this failed, we're screwed.
            Log.wtf(LOGTAG, "Unable to get default profile name.", e);
            throw new RuntimeException(e);
        }
    }

    public static GeckoProfile get(Context context) {
        return get(context, null, null, null);
    }

    public static GeckoProfile get(Context context, String profileName) {
        synchronized (sProfileCache) {
            GeckoProfile profile = sProfileCache.get(profileName);
            if (profile != null)
                return profile;
        }
        return get(context, profileName, (File)null);
    }

    @RobocopTarget
    public static GeckoProfile get(Context context, String profileName, String profilePath) {
        File dir = null;
        if (!TextUtils.isEmpty(profilePath)) {
            dir = new File(profilePath);
            if (!dir.exists() || !dir.isDirectory()) {
                Log.w(LOGTAG, "requested profile directory missing: " + profilePath);
            }
        }
        return get(context, profileName, dir);
    }

    // Extension hook.
    private static volatile BrowserDB.Factory sDBFactory;
    public static void setBrowserDBFactory(BrowserDB.Factory factory) {
        sDBFactory = factory;
    }

    @RobocopTarget
    public static GeckoProfile get(Context context, String profileName, File profileDir) {
        if (sDBFactory == null) {
            // We do this so that GeckoView consumers don't need to know anything about BrowserDB.
            // It's a bit of a broken abstraction, but very tightly coupled, so we work around it
            // for now. We can't just have GeckoView set this, because then it would collide in
            // Fennec's use of GeckoView.
            // We should never see this in Fennec itself, because GeckoApplication sets the factory
            // in onCreate.
            Log.d(LOGTAG, "Defaulting to StubBrowserDB.");
            sDBFactory = StubBrowserDB.getFactory();
        }
        return GeckoProfile.get(context, profileName, profileDir, sDBFactory);
    }

    // Note that the profile cache respects only the profile name!
    // If the directory changes, the returned GeckoProfile instance will be mutated.
    // If the factory differs, it will be *ignored*.
    public static GeckoProfile get(Context context, String profileName, File profileDir, BrowserDB.Factory dbFactory) {
        if (context == null) {
            throw new IllegalArgumentException("context must be non-null");
        }

        // Null name? | Null dir? | Returned profile
        // ------------------------------------------
        //     Yes    |    Yes    | Active profile or default profile.
        //     No     |    Yes    | Profile with specified name at default dir.
        //     Yes    |    No     | Custom (anonymous) profile with specified dir.
        //     No     |    No     | Profile with specified name at specified dir.

        if (profileName == null && profileDir == null) {
            // If no profile info was passed in, look for the active profile or a default profile.
            final GeckoProfile profile = GeckoThread.getActiveProfile();
            if (profile != null) {
                return profile;
            }

            final String args;
            if (context instanceof Activity) {
                args = ContextUtils.getStringExtra(((Activity) context).getIntent(), "args");
            } else {
                args = null;
            }

            final GeckoProfile fromArgs = GeckoProfile.getFromArgs(context, args);
            if (fromArgs != null) {
                return fromArgs;
            }

            // Otherwise, get the default profile for the Activity.
            return getDefaultProfile(context);

        } else if (profileName == null) {
            // If only profile dir was passed in, use custom (anonymous) profile.
            profileName = CUSTOM_PROFILE;

        } else if (AppConstants.DEBUG_BUILD) {
            Log.v(LOGTAG, "Fetching profile: '" + profileName + "', '" + profileDir + "'");
        }

        // Actually try to look up the profile.
        synchronized (sProfileCache) {
            GeckoProfile profile = sProfileCache.get(profileName);
            if (profile == null) {
                try {
                    profile = new GeckoProfile(context, profileName, profileDir, dbFactory);
                } catch (NoMozillaDirectoryException e) {
                    // We're unable to do anything sane here.
                    throw new RuntimeException(e);
                }

                sProfileCache.put(profileName, profile);
                return profile;
            }

            if (profileDir == null) {
                // Fine.
                return profile;
            }

            try {
                if (profile.getDir().getCanonicalPath().equals(profileDir.getCanonicalPath())) {
                    // Great! We're consistent.
                    return profile;
                }
            } catch (final IOException e) {
            }

            if (sAcceptDirectoryChanges && profileDir.isDirectory()) {
                if (AppConstants.RELEASE_BUILD) {
                    Log.e(LOGTAG, "Release build trying to switch out profile dir. This is an error, but let's do what we can.");
                }
                profile.setDir(profileDir);
                return profile;
            }

            throw new IllegalStateException("Refusing to reuse profile with a different directory.");
        }
    }

    // Currently unused outside of testing.
    @RobocopTarget
    public static boolean removeProfile(Context context, String profileName) {
        if (profileName == null) {
            Log.w(LOGTAG, "Unable to remove profile: null profile name.");
            return false;
        }

        final GeckoProfile profile = get(context, profileName);
        if (profile == null) {
            return false;
        }
        final boolean success = profile.remove();

        if (success) {
            // Clear all shared prefs for the given profile.
            GeckoSharedPrefs.forProfileName(context, profileName)
                            .edit().clear().apply();
        }

        return success;
    }

    // Only public for access from tests.
    @RobocopTarget
    public static GeckoProfile createGuestProfile(Context context) {
        try {
            // We need to force the creation of a new guest profile if we want it outside of the normal profile path,
            // otherwise GeckoProfile.getDir will try to be smart and build it for us in the normal profiles dir.
            getGuestDir(context).mkdir();
            sShouldCheckForGuestProfile = true;
            GeckoProfile profile = getGuestProfile(context);

            // If we're creating this guest session over the keyguard, don't lock it.
            // This will force the guest session to exit if the user unlocks their phone
            // and starts Fennec.
            profile.lock();

            /*
             * Now do the things that createProfileDirectory normally does --
             * right now that's kicking off DB init.
             */
            profile.enqueueInitialization(profile.getDir());

            return profile;
        } catch (Exception ex) {
            Log.e(LOGTAG, "Error creating guest profile", ex);
        }
        return null;
    }

    public static void leaveGuestSession(Context context) {
        GeckoProfile profile = getGuestProfile(context);
        if (profile != null) {
            profile.unlock();
        }
    }

    private static File getGuestDir(Context context) {
        if (sGuestDir == null) {
            sGuestDir = context.getFileStreamPath("guest");
        }
        return sGuestDir;
    }

    /**
     * Performs IO. Be careful of using this on the main thread.
     */
    public static GeckoProfile getOrCreateGuestProfile(Context context) {
        GeckoProfile p = getGuestProfile(context);
        if (p == null) {
            return createGuestProfile(context);
        }

        return p;
    }

    public static GeckoProfile getGuestProfile(Context context) {
        if (sGuestProfile == null) {
            if (sShouldCheckForGuestProfile) {
                File guestDir = getGuestDir(context);
                if (guestDir.exists()) {
                    sGuestProfile = get(context, GUEST_PROFILE, guestDir);
                    sGuestProfile.mInGuestMode = true;
                } else {
                    sShouldCheckForGuestProfile = false;
                }
            }
        }

        return sGuestProfile;
    }

    public static boolean maybeCleanupGuestProfile(final Context context) {
        final GeckoProfile profile = getGuestProfile(context);
        if (profile == null) {
            return false;
        }

        if (!profile.locked()) {
            profile.mInGuestMode = false;

            // If the guest dir exists, but it's unlocked, delete it
            removeGuestProfile(context);

            return true;
        }
        return false;
    }

    private static void removeGuestProfile(Context context) {
        boolean success = false;
        try {
            File guestDir = getGuestDir(context);
            if (guestDir.exists()) {
                success = delete(guestDir);
            }
        } catch (Exception ex) {
            Log.e(LOGTAG, "Error removing guest profile", ex);
        }

        if (success) {
            // Clear all shared prefs for the guest profile.
            GeckoSharedPrefs.forProfileName(context, GUEST_PROFILE)
                            .edit().clear().apply();
        }
    }

    public static boolean delete(File file) throws IOException {
        // Try to do a quick initial delete
        if (file.delete())
            return true;

        if (file.isDirectory()) {
            // If the quick delete failed and this is a dir, recursively delete the contents of the dir
            String files[] = file.list();
            for (String temp : files) {
                File fileDelete = new File(file, temp);
                delete(fileDelete);
            }
        }

        // Even if this is a dir, it should now be empty and delete should work
        return file.delete();
    }

    private GeckoProfile(Context context, String profileName, File profileDir, BrowserDB.Factory dbFactory) throws NoMozillaDirectoryException {
        if (profileName == null) {
            throw new IllegalArgumentException("Unable to create GeckoProfile for empty profile name.");
        } else if (CUSTOM_PROFILE.equals(profileName) && profileDir == null) {
            throw new IllegalArgumentException("Custom profile must have a directory");
        }

        mApplicationContext = context.getApplicationContext();
        mName = profileName;
        mMozillaDir = GeckoProfileDirectories.getMozillaDirectory(context);

        mProfileDir = profileDir;
        if (profileDir != null && !profileDir.isDirectory()) {
            throw new IllegalArgumentException("Profile directory must exist if specified.");
        }

        // N.B., mProfileDir can be null at this point.
        mDB = dbFactory.get(profileName, mProfileDir);
    }

    @RobocopTarget
    public BrowserDB getDB() {
        return mDB;
    }

    // Warning, Changing the lock file state from outside apis will cause this to become out of sync
    public boolean locked() {
        if (mLocked != LockState.UNDEFINED) {
            return mLocked == LockState.LOCKED;
        }

        boolean profileExists;
        synchronized (this) {
            profileExists = mProfileDir != null && mProfileDir.exists();
        }

        // Don't use getDir() as it will create a dir if none exists.
        if (profileExists) {
            File lockFile = new File(mProfileDir, LOCK_FILE_NAME);
            boolean res = lockFile.exists();
            mLocked = res ? LockState.LOCKED : LockState.UNLOCKED;
        } else {
            mLocked = LockState.UNLOCKED;
        }

        return mLocked == LockState.LOCKED;
    }

    public boolean lock() {
        try {
            // If this dir doesn't exist getDir will create it for us
            final File lockFile = new File(getDir(), LOCK_FILE_NAME);
            final boolean result = lockFile.createNewFile();
            if (lockFile.exists()) {
                mLocked = LockState.LOCKED;
            } else {
                mLocked = LockState.UNLOCKED;
            }
            return result;
        } catch(IOException ex) {
            Log.e(LOGTAG, "Error locking profile", ex);
        }
        mLocked = LockState.UNLOCKED;
        return false;
    }

    public boolean unlock() {
        final File profileDir;
        synchronized (this) {
            // Don't use getDir() as it will create a dir.
            profileDir = mProfileDir;
        }

        if (profileDir == null || !profileDir.exists()) {
            return true;
        }

        try {
            final File lockFile = new File(profileDir, LOCK_FILE_NAME);
            if (!lockFile.exists()) {
                mLocked = LockState.UNLOCKED;
                return true;
            }

            final boolean result = delete(lockFile);
            if (result) {
                mLocked = LockState.UNLOCKED;
            } else {
                mLocked = LockState.LOCKED;
            }
            return result;
        } catch(IOException ex) {
            Log.e(LOGTAG, "Error unlocking profile", ex);
        }

        mLocked = LockState.LOCKED;
        return false;
    }

    @RobocopTarget
    public boolean inGuestMode() {
        return mInGuestMode;
    }

    private void setDir(File dir) {
        if (dir != null && dir.exists() && dir.isDirectory()) {
            synchronized (this) {
                mProfileDir = dir;
            }
        }
    }

    @RobocopTarget
    public String getName() {
        return mName;
    }

    public boolean isCustomProfile() {
        return CUSTOM_PROFILE.equals(mName);
    }

    @RobocopTarget
    public synchronized File getDir() {
        forceCreate();
        return mProfileDir;
    }

    public synchronized GeckoProfile forceCreate() {
        if (mProfileDir != null) {
            return this;
        }

        try {
            // Check if a profile with this name already exists.
            try {
                mProfileDir = findProfileDir();
                Log.d(LOGTAG, "Found profile dir.");
            } catch (NoSuchProfileException noSuchProfile) {
                // If it doesn't exist, create it.
                mProfileDir = createProfileDir();
            }
        } catch (IOException ioe) {
            Log.e(LOGTAG, "Error getting profile dir", ioe);
        }
        return this;
    }

    public File getFile(String aFile) {
        File f = getDir();
        if (f == null)
            return null;

        return new File(f, aFile);
    }

    /**
     * Retrieves the Gecko client ID from the filesystem. If the client ID does not exist, we attempt to migrate and
     * persist it from FHR and, if that fails, we attempt to create a new one ourselves.
     *
     * This method assumes the client ID is located in a file at a hard-coded path within the profile. The format of
     * this file is a JSONObject which at the bottom level contains a String -> String mapping containing the client ID.
     *
     * WARNING: the platform provides a JSM to retrieve the client ID [1] and this would be a
     * robust way to access it. However, we don't want to rely on Gecko running in order to get
     * the client ID so instead we access the file this module accesses directly. However, it's
     * possible the format of this file (and the access calls in the jsm) will change, leaving
     * this code to fail. There are tests in TestGeckoProfile to verify the file format but be
     * warned: THIS IS NOT FOOLPROOF.
     *
     * [1]: https://mxr.mozilla.org/mozilla-central/source/toolkit/modules/ClientID.jsm
     *
     * @throws IOException if the client ID could not be retrieved.
     */
    // Mimics ClientID.jsm – _doLoadClientID.
    @WorkerThread
    public String getClientId() throws IOException {
        try {
            return getValidClientIdFromDisk(CLIENT_ID_FILE_PATH);
        } catch (final IOException e) {
            // Avoid log spam: don't log the full Exception w/ the stack trace.
            Log.d(LOGTAG, "Could not get client ID - attempting to migrate ID from FHR: " + e.getLocalizedMessage());
        }

        String clientIdToWrite;
        try {
            clientIdToWrite = getValidClientIdFromDisk(FHR_CLIENT_ID_FILE_PATH);
        } catch (final IOException e) {
            // Avoid log spam: don't log the full Exception w/ the stack trace.
            Log.d(LOGTAG, "Could not migrate client ID from FHR – creating a new one: " + e.getLocalizedMessage());
            clientIdToWrite = UUID.randomUUID().toString();
        }

        // There is a possibility Gecko is running and the Gecko telemetry implementation decided it's time to generate
        // the client ID, writing client ID underneath us. Since it's highly unlikely (e.g. we run in onStart before
        // Gecko is started), we don't handle that possibility besides writing the ID and then reading from the file
        // again (rather than just returning the value we generated before writing).
        //
        // In the event it does happen, any discrepancy will be resolved after a restart. In the mean time, both this
        // implementation and the Gecko implementation could upload documents with inconsistent IDs.
        //
        // In any case, if we get an exception, intentionally throw - there's nothing more to do here.
        persistClientId(clientIdToWrite);
        return getValidClientIdFromDisk(CLIENT_ID_FILE_PATH);
    }

    /**
     * @return a valid client ID
     * @throws IOException if a valid client ID could not be retrieved
     */
    @WorkerThread
    private String getValidClientIdFromDisk(final String filePath) throws IOException {
        final JSONObject obj = readJSONObjectFromFile(filePath);
        final String clientId = obj.optString(CLIENT_ID_JSON_ATTR);
        if (isClientIdValid(clientId)) {
            return clientId;
        }
        throw new IOException("Received client ID is invalid: " + clientId);
    }

    @WorkerThread
    private void persistClientId(final String clientId) throws IOException {
        if (!ensureParentDirs(CLIENT_ID_FILE_PATH)) {
            throw new IOException("Could not create client ID parent directories");
        }

        final JSONObject obj = new JSONObject();
        try {
            obj.put(CLIENT_ID_JSON_ATTR, clientId);
        } catch (final JSONException e) {
            throw new IOException("Could not create client ID JSON object", e);
        }

        // ClientID.jsm overwrites the file to store the client ID so it's okay if we do it too.
        Log.d(LOGTAG, "Attempting to write new client ID");
        writeFile(CLIENT_ID_FILE_PATH, obj.toString()); // Logs errors within function: ideally we'd throw.
    }

    // From ClientID.jsm - isValidClientID.
    public static boolean isClientIdValid(final String clientId) {
        // We could use UUID.fromString but, for consistency, we take the implementation from ClientID.jsm.
        if (TextUtils.isEmpty(clientId)) {
            return false;
        }
        return clientId.matches("(?i:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})");
    }

    /**
     * Gets the profile creation date and persists it if it had to be generated.
     *
     * To get this value, we first look in times.json. If that could not be accessed, we
     * return the package's first install date. This is not a perfect solution because a
     * user may have large gap between install time and first use.
     *
     * A more correct algorithm could be the one performed by the JS code in ProfileAge.jsm
     * getOldestProfileTimestamp: walk the tree and return the oldest timestamp on the files
     * within the profile. However, since times.json will only not exist for the small
     * number of really old profiles, we're okay with the package install date compromise for
     * simplicity.
     *
     * @return the profile creation date in the format returned by {@link System#currentTimeMillis()}
     *         or -1 if the value could not be persisted.
     */
    @WorkerThread
    public long getAndPersistProfileCreationDate(final Context context) {
        try {
            return getProfileCreationDateFromTimesFile();
        } catch (final IOException e) {
            Log.d(LOGTAG, "Unable to retrieve profile creation date from times.json. Getting from system...");
            final long packageInstallMillis = org.mozilla.gecko.util.ContextUtils.getPackageInstallTime(context);
            try {
                persistProfileCreationDateToTimesFile(packageInstallMillis);
            } catch (final IOException ioEx) {
                // We return -1 to ensure the profileCreationDate
                // will either be an error (-1) or a consistent value.
                Log.w(LOGTAG, "Unable to persist profile creation date - returning -1");
                return -1;
            }

            return packageInstallMillis;
        }
    }

    @WorkerThread
    private long getProfileCreationDateFromTimesFile() throws IOException {
        final JSONObject obj = readJSONObjectFromFile(TIMES_PATH);
        try {
            return obj.getLong(PROFILE_CREATION_DATE_JSON_ATTR);
        } catch (final JSONException e) {
            // Don't log to avoid leaking data in JSONObject.
            throw new IOException("Profile creation does not exist in JSONObject");
        }
    }

    @WorkerThread
    private void persistProfileCreationDateToTimesFile(final long profileCreationMillis) throws IOException {
        final JSONObject obj = new JSONObject();
        try {
            obj.put(PROFILE_CREATION_DATE_JSON_ATTR, profileCreationMillis);
        } catch (final JSONException e) {
            // Don't log to avoid leaking data in JSONObject.
            throw new IOException("Unable to persist profile creation date to times file");
        }
        Log.d(LOGTAG, "Attempting to write new profile creation date");
        writeFile(TIMES_PATH, obj.toString()); // Ideally we'd throw here too.
    }

    /**
     * Moves the session file to the backup session file.
     *
     * sessionstore.js should hold the current session, and sessionstore.bak
     * should hold the previous session (where it is used to read the "tabs
     * from last time"). Normally, sessionstore.js is moved to sessionstore.bak
     * on a clean quit, but this doesn't happen if Fennec crashed. Thus, this
     * method should be called after a crash so sessionstore.bak correctly
     * holds the previous session.
     */
    public void moveSessionFile() {
        File sessionFile = getFile("sessionstore.js");
        if (sessionFile != null && sessionFile.exists()) {
            File sessionFileBackup = getFile("sessionstore.bak");
            sessionFile.renameTo(sessionFileBackup);
        }
    }

    /**
     * Get the string from a session file.
     *
     * The session can either be read from sessionstore.js or sessionstore.bak.
     * In general, sessionstore.js holds the current session, and
     * sessionstore.bak holds the previous session.
     *
     * @param readBackup if true, the session is read from sessionstore.bak;
     *                   otherwise, the session is read from sessionstore.js
     *
     * @return the session string
     */
    public String readSessionFile(boolean readBackup) {
        File sessionFile = getFile(readBackup ? "sessionstore.bak" : "sessionstore.js");

        try {
            if (sessionFile != null && sessionFile.exists()) {
                return readFile(sessionFile);
            }
        } catch (IOException ioe) {
            Log.e(LOGTAG, "Unable to read session file", ioe);
        }
        return null;
    }

    /**
     * Ensures the parent director(y|ies) of the given filename exist by making them
     * if they don't already exist..
     *
     * @param filename The path to the file whose parents should be made directories
     * @return true if the parent directory exists, false otherwise
     */
    @WorkerThread
    protected boolean ensureParentDirs(final String filename) {
        final File file = new File(getDir(), filename);
        final File parentFile = file.getParentFile();
        return parentFile.mkdirs() || parentFile.isDirectory();
    }

    public void writeFile(final String filename, final String data) {
        File file = new File(getDir(), filename);
        BufferedWriter bufferedWriter = null;
        try {
            bufferedWriter = new BufferedWriter(new FileWriter(file, false));
            bufferedWriter.write(data);
        } catch (IOException e) {
            Log.e(LOGTAG, "Unable to write to file", e);
        } finally {
            try {
                if (bufferedWriter != null) {
                    bufferedWriter.close();
                }
            } catch (IOException e) {
                Log.e(LOGTAG, "Error closing writer while writing to file", e);
            }
        }
    }

    @WorkerThread
    public JSONObject readJSONObjectFromFile(final String filename) throws IOException {
        final String fileContents;
        try {
            fileContents = readFile(filename);
        } catch (final IOException e) {
            // Don't log exception to avoid leaking profile path.
            throw new IOException("Could not access given file to retrieve JSONObject");
        }

        try {
            return new JSONObject(fileContents);
        } catch (final JSONException e) {
            // Don't log exception to avoid leaking profile path.
            throw new IOException("Could not parse JSON to retrieve JSONObject");
        }
    }

    public JSONArray readJSONArrayFromFile(final String filename) {
        String fileContent;
        try {
            fileContent = readFile(filename);
        } catch (IOException expected) {
            return new JSONArray();
        }

        JSONArray jsonArray;
        try {
            jsonArray = new JSONArray(fileContent);
        } catch (JSONException e) {
            jsonArray = new JSONArray();
        }
        return jsonArray;
    }

    public String readFile(String filename) throws IOException {
        File dir = getDir();
        if (dir == null) {
            throw new IOException("No profile directory found");
        }
        File target = new File(dir, filename);
        return readFile(target);
    }

    private String readFile(File target) throws IOException {
        FileReader fr = new FileReader(target);
        try {
            StringBuilder sb = new StringBuilder();
            char[] buf = new char[8192];
            int read = fr.read(buf);
            while (read >= 0) {
                sb.append(buf, 0, read);
                read = fr.read(buf);
            }
            return sb.toString();
        } finally {
            fr.close();
        }
    }

    public boolean deleteFileFromProfileDir(String fileName) throws IllegalArgumentException {
        if (TextUtils.isEmpty(fileName)) {
            throw new IllegalArgumentException("Filename cannot be empty.");
        }
        File file = new File(getDir(), fileName);
        return file.delete();
    }

    private boolean remove() {
        try {
            synchronized (this) {
                final File dir = getDir();
                if (dir.exists()) {
                    delete(dir);
                }

                try {
                    mProfileDir = findProfileDir();
                } catch (NoSuchProfileException noSuchProfile) {
                    // If the profile doesn't exist, there's nothing left for us to do.
                    return false;
                }
            }

            final INIParser parser = GeckoProfileDirectories.getProfilesINI(mMozillaDir);
            final Hashtable<String, INISection> sections = parser.getSections();
            for (Enumeration<INISection> e = sections.elements(); e.hasMoreElements();) {
                final INISection section = e.nextElement();
                String name = section.getStringProperty("Name");

                if (name == null || !name.equals(mName)) {
                    continue;
                }

                if (section.getName().startsWith("Profile")) {
                    // ok, we have stupid Profile#-named things.  Rename backwards.
                    try {
                        int sectionNumber = Integer.parseInt(section.getName().substring("Profile".length()));
                        String curSection = "Profile" + sectionNumber;
                        String nextSection = "Profile" + (sectionNumber+1);

                        sections.remove(curSection);

                        while (sections.containsKey(nextSection)) {
                            parser.renameSection(nextSection, curSection);
                            sectionNumber++;

                            curSection = nextSection;
                            nextSection = "Profile" + (sectionNumber+1);
                        }
                    } catch (NumberFormatException nex) {
                        // uhm, malformed Profile thing; we can't do much.
                        Log.e(LOGTAG, "Malformed section name in profiles.ini: " + section.getName());
                        return false;
                    }
                } else {
                    // this really shouldn't be the case, but handle it anyway
                    parser.removeSection(mName);
                }

                break;
            }

            parser.write();
            return true;
        } catch (IOException ex) {
            Log.w(LOGTAG, "Failed to remove profile.", ex);
            return false;
        }
    }

    /**
     * @return the default profile name for this application, or
     *         {@link GeckoProfile#DEFAULT_PROFILE} if none could be found.
     *
     * @throws NoMozillaDirectoryException
     *             if the Mozilla directory did not exist and could not be
     *             created.
     */
    public static String getDefaultProfileName(final Context context) throws NoMozillaDirectoryException {
        // Have we read the default profile from the INI already?
        // Changing the default profile requires a restart, so we don't
        // need to worry about runtime changes.
        if (sDefaultProfileName != null) {
            return sDefaultProfileName;
        }

        final String profileName = GeckoProfileDirectories.findDefaultProfileName(context);
        if (profileName == null) {
            // Note that we don't persist this back to profiles.ini.
            sDefaultProfileName = DEFAULT_PROFILE;
            return DEFAULT_PROFILE;
        }

        sDefaultProfileName = profileName;
        return sDefaultProfileName;
    }

    private File findProfileDir() throws NoSuchProfileException {
        if (isCustomProfile()) {
            return mProfileDir;
        }
        return GeckoProfileDirectories.findProfileDir(mMozillaDir, mName);
    }

    private File createProfileDir() throws IOException {
        if (isCustomProfile()) {
            // Custom profiles must already exist.
            return mProfileDir;
        }

        INIParser parser = GeckoProfileDirectories.getProfilesINI(mMozillaDir);

        // Salt the name of our requested profile
        String saltedName = GeckoProfileDirectories.saltProfileName(mName);
        File profileDir = new File(mMozillaDir, saltedName);
        while (profileDir.exists()) {
            saltedName = GeckoProfileDirectories.saltProfileName(mName);
            profileDir = new File(mMozillaDir, saltedName);
        }

        // Attempt to create the salted profile dir
        if (!profileDir.mkdirs()) {
            throw new IOException("Unable to create profile.");
        }
        Log.d(LOGTAG, "Created new profile dir.");

        // Now update profiles.ini
        // If this is the first time its created, we also add a General section
        // look for the first profile number that isn't taken yet
        int profileNum = 0;
        boolean isDefaultSet = false;
        INISection profileSection;
        while ((profileSection = parser.getSection("Profile" + profileNum)) != null) {
            profileNum++;
            if (profileSection.getProperty("Default") != null) {
                isDefaultSet = true;
            }
        }

        profileSection = new INISection("Profile" + profileNum);
        profileSection.setProperty("Name", mName);
        profileSection.setProperty("IsRelative", 1);
        profileSection.setProperty("Path", saltedName);

        if (parser.getSection("General") == null) {
            INISection generalSection = new INISection("General");
            generalSection.setProperty("StartWithLastProfile", 1);
            parser.addSection(generalSection);
        }

        if (!isDefaultSet) {
            // only set as default if this is the first profile we're creating
            profileSection.setProperty("Default", 1);

            // We have no intention of stopping this session. The FIRSTRUN session
            // ends when the browsing session/activity has ended. All events
            // during firstrun will be tagged as FIRSTRUN.
            Telemetry.startUISession(TelemetryContract.Session.FIRSTRUN);
        }

        parser.addSection(profileSection);
        parser.write();

        enqueueInitialization(profileDir);

        // Write out profile creation time, mirroring the logic in nsToolkitProfileService.
        try {
            FileOutputStream stream = new FileOutputStream(profileDir.getAbsolutePath() + File.separator + "times.json");
            OutputStreamWriter writer = new OutputStreamWriter(stream, Charset.forName("UTF-8"));
            try {
                writer.append("{\"created\": " + System.currentTimeMillis() + "}\n");
            } finally {
                writer.close();
            }
        } catch (Exception e) {
            // Best-effort.
            Log.w(LOGTAG, "Couldn't write times.json.", e);
        }

        // Initialize pref flag for displaying the start pane for a new profile.
        final SharedPreferences prefs = GeckoSharedPrefs.forProfile(mApplicationContext);
        prefs.edit().putBoolean(FirstrunAnimationContainer.PREF_FIRSTRUN_ENABLED, true).apply();

        return profileDir;
    }

    /**
     * This method is called once, immediately before creation of the profile
     * directory completes.
     *
     * It queues up work to be done in the background to prepare the profile,
     * such as adding default bookmarks.
     *
     * This is public for use *from tests only*!
     */
    @RobocopTarget
    public void enqueueInitialization(final File profileDir) {
        Log.i(LOGTAG, "Enqueuing profile init.");
        final Context context = mApplicationContext;

        // Add everything when we're done loading the distribution.
        final Distribution distribution = Distribution.getInstance(context);
        distribution.addOnDistributionReadyCallback(new Distribution.ReadyCallback() {
            @Override
            public void distributionNotFound() {
                this.distributionFound(null);
            }

            @Override
            public void distributionFound(Distribution distribution) {
                Log.d(LOGTAG, "Running post-distribution task: bookmarks.");

                final ContentResolver cr = context.getContentResolver();

                // Because we are running in the background, we want to synchronize on the
                // GeckoProfile instance so that we don't race with main thread operations
                // such as locking/unlocking/removing the profile.
                synchronized (GeckoProfile.this) {
                    // Skip initialization if the profile directory has been removed.
                    if (!profileDir.exists()) {
                        return;
                    }

                    // We pass the number of added bookmarks to ensure that the
                    // indices of the distribution and default bookmarks are
                    // contiguous. Because there are always at least as many
                    // bookmarks as there are favicons, we can also guarantee that
                    // the favicon IDs won't overlap.
                    final LocalBrowserDB db = new LocalBrowserDB(getName());
                    final int offset = distribution == null ? 0 : db.addDistributionBookmarks(cr, distribution, 0);
                    db.addDefaultBookmarks(context, cr, offset);

                    Log.d(LOGTAG, "Running post-distribution task: android preferences.");
                    DistroSharedPrefsImport.importPreferences(context, distribution);
                }
            }

            @Override
            public void distributionArrivedLate(Distribution distribution) {
                Log.d(LOGTAG, "Running late distribution task: bookmarks.");
                // Recover as best we can.
                synchronized (GeckoProfile.this) {
                    // Skip initialization if the profile directory has been removed.
                    if (!profileDir.exists()) {
                        return;
                    }

                    final LocalBrowserDB db = new LocalBrowserDB(getName());
                    // We assume we've been called very soon after startup, and so our offset
                    // into "Mobile Bookmarks" is the number of bookmarks in the DB.
                    final ContentResolver cr = context.getContentResolver();
                    final int offset = db.getCount(cr, "bookmarks");
                    db.addDistributionBookmarks(cr, distribution, offset);

                    Log.d(LOGTAG, "Running late distribution task: android preferences.");
                    DistroSharedPrefsImport.importPreferences(context, distribution);
                }
            }
        });
    }
}
