package com.megaman64recomp.vr;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends SDLActivity {
    private static final String TAG = "MegaMan64";

    @Override
    protected String[] getLibraries() {
        // The game's other dependencies (FreeType, the OpenXR loader...) are loaded along with it.
        return new String[] { "c++_shared", "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        extractGameAssets();
        // Creates the external files folder (config, saves and the ROM go there).
        getExternalFilesDir(null);
        super.onCreate(savedInstanceState);
    }

    // Copies the UI assets and the controller database out of the APK into internal storage, which is the
    // game's working directory. Only done again when the APK changes.
    private void extractGameAssets() {
        File root = getFilesDir();
        File marker = new File(root, ".assets_version");
        String version = Long.toString(getApkLastUpdateTime());
        if (marker.exists() && version.equals(readText(marker))) {
            return;
        }
        try {
            copyAssetTree(getAssets(), "assets", new File(root, "assets"));
            copyAsset(getAssets(), "recompcontrollerdb.txt", new File(root, "recompcontrollerdb.txt"));
            writeText(marker, version);
        } catch (IOException e) {
            Log.e(TAG, "Failed to extract game assets", e);
        }
    }

    private long getApkLastUpdateTime() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (Exception e) {
            return 0;
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            copyAsset(assets, path, target);
            return;
        }
        target.mkdirs();
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(target, child));
        }
    }

    private static void copyAsset(AssetManager assets, String path, File target) throws IOException {
        target.getParentFile().mkdirs();
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }

    private static String readText(File file) {
        try (InputStream in = new java.io.FileInputStream(file)) {
            byte[] data = new byte[(int) file.length()];
            int read = in.read(data);
            return new String(data, 0, Math.max(read, 0));
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes());
        }
    }
}
