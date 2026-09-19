package com.zhixiaoban.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.webkit.PermissionRequest;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * 植小伴 手机端 App —— 轻量外壳，内部加载服务器上的 /mobile 页面。
 * 界面与数据全部来自服务器，换环境只要在"连不上"页面改一下地址即可。
 */
public class MainActivity extends Activity {

    private static final String PREFS = "zxb";
    private static final String KEY_URL = "server_url";
    private static final String DEFAULT_URL = "http://YOUR_SERVER_HOST:8011/mobile/";
    private static final int REQ_FILE = 1001;

    private WebView web;
    private View offlineView;
    private EditText offlineInput;
    private ValueCallback<Uri[]> fileCallback;
    private Uri cameraUri;
    private long lastBackPress;
    private volatile String baseUrl = "";

    /* ── 语音输入：原生采 16k 单声道 PCM，交服务器转文字 ── */
    private static final int REQ_MIC = 1002;
    private static final int VOICE_RATE = 16000;
    private static final int VOICE_MAX_S = 20;        // 一次最多录 20 秒
    private static final int VOICE_MIN_BYTES = 6400;  // 少于 0.2 秒＝没说话
    private static final String VOICE_PATH = "/api/v1/voice/transcribe";

    private final Object recLock = new Object();
    private volatile AudioRecord recorder;
    private Thread recThread;
    private volatile boolean recWant;
    private boolean recFinishing;
    private ByteArrayOutputStream recBuf;
    private volatile String recToken = "";

    /* ── 原生 TTS：把 AI 回复念出来 ──
     * 为什么用系统语音而不是让服务器合成：服务器合成要等大模型返回音频
     * （实测 2.6~5 秒），小朋友问一句要干等；系统 TTS 是本地引擎，几乎
     * 瞬时出声，也不吃流量。手机没装中文语音包时 ttsOk=false，网页会自动
     * 退回服务器的 /voice/speak，功能不丢。
     * 网页侧接口：window.ZXB.ttsReady() / speak(text) / stopSpeak()。 */
    private TextToSpeech tts;
    private volatile boolean ttsOk = false;

    private void setupTts() {
        try {
            tts = new TextToSpeech(getApplicationContext(),
                    new TextToSpeech.OnInitListener() {
                @Override
                public void onInit(int status) {
                    if (status != TextToSpeech.SUCCESS || tts == null) {
                        ttsOk = false;
                        return;
                    }
                    int r = tts.setLanguage(Locale.SIMPLIFIED_CHINESE);
                    if (r == TextToSpeech.LANG_MISSING_DATA
                            || r == TextToSpeech.LANG_NOT_SUPPORTED) {
                        /* 没中文包：能用的语言也凑合，全不行就交给服务器 */
                        int r2 = tts.setLanguage(Locale.getDefault());
                        ttsOk = (r2 != TextToSpeech.LANG_MISSING_DATA
                                 && r2 != TextToSpeech.LANG_NOT_SUPPORTED);
                    } else {
                        ttsOk = true;
                    }
                    if (ttsOk) {
                        tts.setSpeechRate(1.0f);
                        tts.setPitch(1.0f);
                    }
                }
            });
        } catch (Exception e) {
            tts = null;
            ttsOk = false;
        }
    }

    /* ───────────── 服务器地址读写 ───────────── */

    public static String serverUrl(Context c) {
        SharedPreferences sp = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String u = sp.getString(KEY_URL, DEFAULT_URL);
        if (u == null || u.trim().length() == 0) u = DEFAULT_URL;
        u = u.trim();
        if (!u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
        if (!u.endsWith("/")) u = u + "/";
        return u;
    }

    private void saveUrl(String u) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_URL, u).apply();
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    /* ───────────── 生命周期 ───────────── */

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            getWindow().setStatusBarColor(Color.parseColor("#14532D"));
            getWindow().setNavigationBarColor(Color.parseColor("#F0FDF4"));
        }

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.parseColor("#F0FDF4"));

        web = new WebView(this);
        root.addView(web, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        offlineView = buildOfflineView();
        root.addView(offlineView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        setContentView(root);
        setupWebView();
        setupTts();

        /* 每次启动清一次 WebView 的 HTTP 缓存。
         * 踩过的坑：/mobile/ 静态资源原来没有 Cache-Control，WebView 按
         * "启发式缓存"把旧 app.js 长期留在本地复用 —— 服务器改了前端，
         * 手机上完全看不到变化，重启 App 也没用（Service Worker 在 http
         * 非安全上下文下不注册，兜不住）。服务器侧已发 no-store，这里再
         * 兜一道，保证每次开 App 拿到的都是最新前端。 */
        try { web.clearCache(true); } catch (Exception ignored) { }

        load(serverUrl(this));
    }

    @Override
    protected void onDestroy() {
        try {
            if (tts != null) {
                tts.stop();
                tts.shutdown();
                tts = null;
            }
        } catch (Exception ignored) {
        }
        super.onDestroy();
    }

    private void setupWebView() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setUseWideViewPort(true);
        s.setLoadWithOverviewMode(true);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setDisplayZoomControls(false);
        s.setAllowFileAccess(true);
        s.setJavaScriptCanOpenWindowsAutomatically(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setUserAgentString(s.getUserAgentString() + " ZhiXiaoBanApp/1.0");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            WebView.setWebContentsDebuggingEnabled(true);
        }

        web.addJavascriptInterface(new Object() {
            @JavascriptInterface
            public boolean isApp() {
                return true;
            }

            @JavascriptInterface
            public void openSettings() {
                runOnUiThread(new Runnable() {
                    public void run() {
                        showOffline("在这里填写服务器地址");
                    }
                });
            }

            @JavascriptInterface
            public boolean hasVoice() {
                return getPackageManager().hasSystemFeature(PackageManager.FEATURE_MICROPHONE);
            }

            /** 开始录音。返回 "ok"＝已在录，"ask"＝正在申请麦克风权限，"no"＝不可用。 */
            @JavascriptInterface
            public String startVoice(String token) {
                recToken = (token == null) ? "" : token;
                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                        != PackageManager.PERMISSION_GRANTED) {
                    runOnUiThread(new Runnable() {
                        public void run() {
                            requestPermissions(
                                    new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
                        }
                    });
                    return "ask";
                }
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (startRecorder()) jsVoiceEvent("started", null, null);
                        else jsVoiceEvent("error", null, "打不开麦克风，请检查手机的麦克风权限");
                    }
                });
                return "ok";
            }

            /** 结束录音 → 上传 → 转文字（结果用 window.__zxbVoice 回调网页）。 */
            @JavascriptInterface
            public void stopVoice() {
                new Thread(new Runnable() {
                    public void run() { finishVoice(); }
                }, "zxb-voice-stop").start();
            }

            /** 放弃这次录音（例如用户离开了问答页）。 */
            @JavascriptInterface
            public void cancelVoice() {
                new Thread(new Runnable() {
                    public void run() { discardVoice(); }
                }, "zxb-voice-cancel").start();
            }

            /** 系统语音引擎是否可用（不可用时网页退回服务器的 TTS）。 */
            @JavascriptInterface
            public boolean ttsReady() {
                return ttsOk;
            }

            /** 念一段话（本地引擎，瞬时出声；后一句会打断前一句）。 */
            @JavascriptInterface
            public void speak(String text) {
                if (!ttsOk || tts == null || text == null) return;
                final String s = text.trim();
                if (s.length() == 0) return;
                runOnUiThread(new Runnable() {
                    public void run() {
                        try {
                            tts.speak(s, TextToSpeech.QUEUE_FLUSH, null, "zxb");
                        } catch (Exception ignored) {
                        }
                    }
                });
            }

            /** 停止朗读（切页、或用户点了静音）。 */
            @JavascriptInterface
            public void stopSpeak() {
                if (tts == null) return;
                runOnUiThread(new Runnable() {
                    public void run() {
                        try { tts.stop(); } catch (Exception ignored) { }
                    }
                });
            }
        }, "ZXB");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, String url) {
                if (url != null && (url.startsWith("http://") || url.startsWith("https://"))) {
                    return false;
                }
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
                } catch (Exception e) {
                    /* 忽略无法处理的链接 */
                }
                return true;
            }

            @Override
            public void onPageFinished(WebView v, String url) {
                if (url != null && url.startsWith("http")) hideOffline();
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest req, WebResourceError err) {
                if (req != null && req.isForMainFrame()) {
                    showOffline("连不上服务器");
                }
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onReceivedError(WebView v, int code, String desc, String failingUrl) {
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                    showOffline("连不上服务器");
                }
            }
        });

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView v, ValueCallback<Uri[]> cb,
                                             FileChooserParams params) {
                if (fileCallback != null) {
                    fileCallback.onReceiveValue(null);
                }
                fileCallback = cb;
                askImageSource();
                return true;
            }

            @Override
            public void onPermissionRequest(final PermissionRequest request) {
                // 页面只来自我们自己的服务器，录音/拍照请求一律放行
                runOnUiThread(new Runnable() {
                    public void run() {
                        try {
                            request.grant(request.getResources());
                        } catch (Exception ignored) {
                        }
                    }
                });
            }
        });
    }

    private void load(String url) {
        offlineInput.setText(url);
        baseUrl = originOf(url);
        web.loadUrl(url);
    }

    /* ───────────── 图片选择（拍照 / 相册） ───────────── */

    private void askImageSource() {
        final String[] items = new String[]{"拍照", "从相册选择", "取消"};
        new AlertDialog.Builder(this)
                .setTitle("添加照片")
                .setItems(items, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        if (which == 0) takePhoto();
                        else if (which == 1) pickPhoto();
                        else finishChooser(null);
                    }
                })
                .setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override
                    public void onCancel(DialogInterface d) {
                        finishChooser(null);
                    }
                })
                .show();
    }

    private void takePhoto() {
        try {
            File dir = SimpleFileProvider.sharedDir(this);
            File f = new File(dir, "cap_" + System.currentTimeMillis() + ".jpg");
            cameraUri = SimpleFileProvider.uriFor(this, f);
            Intent i = new Intent("android.media.action.IMAGE_CAPTURE");
            i.putExtra("output", cameraUri);
            i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(i, REQ_FILE);
        } catch (Exception e) {
            Toast.makeText(this, "打不开相机", Toast.LENGTH_SHORT).show();
            finishChooser(null);
        }
    }

    private void pickPhoto() {
        try {
            Intent i = new Intent(Intent.ACTION_GET_CONTENT);
            i.setType("image/*");
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(Intent.createChooser(i, "选择照片"), REQ_FILE);
        } catch (Exception e) {
            Toast.makeText(this, "打不开相册", Toast.LENGTH_SHORT).show();
            finishChooser(null);
        }
    }

    private void finishChooser(Uri[] result) {
        if (fileCallback != null) {
            fileCallback.onReceiveValue(result);
            fileCallback = null;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_FILE) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        Uri[] result = null;
        if (resultCode == RESULT_OK) {
            if (data == null || (data.getData() == null && data.getClipData() == null)) {
                if (cameraUri != null) result = new Uri[]{cameraUri};
            } else if (data.getClipData() != null) {
                int n = data.getClipData().getItemCount();
                result = new Uri[n];
                for (int i = 0; i < n; i++) result[i] = data.getClipData().getItemAt(i).getUri();
            } else {
                result = new Uri[]{data.getData()};
            }
        }
        finishChooser(result);
    }

    /* ───────────── 语音输入：原生采音 → 服务器转文字 ───────────── */

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        super.onRequestPermissionsResult(req, perms, res);
        if (req != REQ_MIC) return;
        boolean ok = (res != null && res.length > 0
                && res[0] == PackageManager.PERMISSION_GRANTED);
        if (!ok) {
            jsVoiceEvent("denied", null, null);
            return;
        }
        // 授权了就把刚才那次"开始说话"补上
        if (startRecorder()) jsVoiceEvent("started", null, null);
        else jsVoiceEvent("error", null, "打不开麦克风，请检查手机的麦克风权限");
    }

    private boolean startRecorder() {
        synchronized (recLock) {
            if (recorder != null || recFinishing) return recorder != null;
            try {
                int min = AudioRecord.getMinBufferSize(VOICE_RATE,
                        AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
                if (min <= 0) min = VOICE_RATE * 2;
                int bufSize = Math.max(min, VOICE_RATE);   // 至少 0.5 秒，避免欠载
                // VOICE_RECOGNITION：关掉为放音乐准备的自动增益，语音识别更准
                recorder = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                        VOICE_RATE, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, bufSize);
                if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                    releaseRecorder();
                    return false;
                }
                recBuf = new ByteArrayOutputStream();
                recWant = true;
                recorder.startRecording();
            } catch (Exception e) {
                releaseRecorder();
                return false;
            }
        }
        recThread = new Thread(new Runnable() {
            public void run() { readLoop(); }
        }, "zxb-voice");
        recThread.start();
        return true;
    }

    private void readLoop() {
        byte[] buf = new byte[VOICE_RATE / 10 * 2];        // 100ms 一块
        int maxBytes = VOICE_RATE * 2 * VOICE_MAX_S;
        while (recWant) {
            int n;
            try {
                n = recorder.read(buf, 0, buf.length);
            } catch (Exception e) {
                break;
            }
            if (n <= 0) break;
            synchronized (recLock) {
                if (recBuf == null) break;
                recBuf.write(buf, 0, n);
                if (recBuf.size() >= maxBytes) break;
            }
        }
        // 录满自动收尾（本线程已经退出循环，不会自己 join 自己）
        if (recWant) finishVoice();
    }

    /** 结束录音并把 PCM 交给服务器转文字。可能跑在 worker 线程上。 */
    private void finishVoice() {
        byte[] pcm;
        synchronized (recLock) {
            if (recorder == null || recFinishing) return;
            recFinishing = true;
            recWant = false;
            try {
                recorder.stop();
            } catch (Exception ignored) {
            }
        }
        Thread t = recThread;
        recThread = null;
        if (t != null && t != Thread.currentThread()) {
            try {
                t.join(1500);
            } catch (InterruptedException ignored) {
            }
        }
        synchronized (recLock) {
            pcm = (recBuf == null) ? new byte[0] : recBuf.toByteArray();
            recBuf = null;
            releaseRecorder();
            recFinishing = false;
        }
        if (pcm.length < VOICE_MIN_BYTES) {
            jsVoiceEvent("error", null, "说得太短啦，多说两句再点结束");
            return;
        }
        uploadVoice(pcm, recToken);
    }

    /** 丢弃录音，不上传。 */
    private void discardVoice() {
        synchronized (recLock) {
            if (recorder == null && recBuf == null) return;
            recWant = false;
            try {
                if (recorder != null) recorder.stop();
            } catch (Exception ignored) {
            }
        }
        Thread t = recThread;
        recThread = null;
        if (t != null && t != Thread.currentThread()) {
            try {
                t.join(1000);
            } catch (InterruptedException ignored) {
            }
        }
        synchronized (recLock) {
            recBuf = null;
            releaseRecorder();
            recFinishing = false;
        }
    }

    private void releaseRecorder() {
        if (recorder != null) {
            try {
                recorder.release();
            } catch (Exception ignored) {
            }
            recorder = null;
        }
    }

    /** 裸 PCM16@16k 单声道直接 POST 上去，服务器只做转写、不做回答。 */
    private void uploadVoice(final byte[] pcm, final String token) {
        final String base = (baseUrl != null && baseUrl.length() > 0)
                ? baseUrl : originOf(serverUrl(this));
        new Thread(new Runnable() {
            public void run() {
                HttpURLConnection c = null;
                try {
                    c = (HttpURLConnection) new URL(base + VOICE_PATH).openConnection();
                    c.setRequestMethod("POST");
                    c.setConnectTimeout(8000);
                    c.setReadTimeout(60000);
                    c.setDoOutput(true);
                    c.setFixedLengthStreamingMode(pcm.length);
                    c.setRequestProperty("Content-Type", "application/octet-stream");
                    if (token != null && token.length() > 0) {
                        c.setRequestProperty("Authorization", "Bearer " + token);
                    }
                    OutputStream os = c.getOutputStream();
                    os.write(pcm);
                    os.flush();
                    os.close();
                    int code = c.getResponseCode();
                    InputStream is = (code >= 400) ? c.getErrorStream() : c.getInputStream();
                    String body = readAll(is);
                    if (code == 200) {
                        jsVoiceEvent("text", new JSONObject(body).optString("text", ""), null);
                    } else {
                        jsVoiceEvent("error", null, detailOf(body, code));
                    }
                } catch (Exception e) {
                    jsVoiceEvent("error", null, "网络不太好，请再试一次");
                } finally {
                    if (c != null) c.disconnect();
                }
            }
        }, "zxb-voice-up").start();
    }

    private static String readAll(InputStream is) {
        if (is == null) return "";
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        try {
            byte[] b = new byte[4096];
            int n;
            while ((n = is.read(b)) > 0) bo.write(b, 0, n);
            is.close();
        } catch (Exception ignored) {
        }
        return new String(bo.toByteArray(), StandardCharsets.UTF_8);
    }

    private static String detailOf(String body, int code) {
        try {
            String d = new JSONObject(body).optString("detail", "");
            if (d.length() > 0) return d;
        } catch (Exception ignored) {
        }
        return "识别失败（HTTP " + code + "）";
    }

    /** 回调网页：window.__zxbVoice({event:..., text:..., error:...})。 */
    private void jsVoiceEvent(String event, String text, String error) {
        StringBuilder j = new StringBuilder();
        j.append('{').append(jsonStr("event")).append(':').append(jsonStr(event));
        if (text != null) {
            j.append(',').append(jsonStr("text")).append(':').append(jsonStr(text));
        }
        if (error != null) {
            j.append(',').append(jsonStr("error")).append(':').append(jsonStr(error));
        }
        j.append('}');
        final String expr = "window.__zxbVoice && window.__zxbVoice(" + j + ")";
        web.post(new Runnable() {
            public void run() {
                try {
                    web.evaluateJavascript(expr, null);
                } catch (Exception ignored) {
                }
            }
        });
    }

    private static String jsonStr(String v) {
        if (v == null) return "null";
        StringBuilder b = new StringBuilder();
        b.append('"');
        for (int i = 0; i < v.length(); i++) {
            char c = v.charAt(i);
            if (c == '"') b.append("\\\"");
            else if (c == '\\') b.append("\\\\");
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c == '\t') b.append("\\t");
            else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        b.append('"');
        return b.toString();
    }

    /** 从 http://host:8011/mobile/ 里取出 http://host:8011。 */
    private static String originOf(String url) {
        if (url == null) return "";
        try {
            URI uri = URI.create(url.trim());
            String host = uri.getHost();
            if (host != null) {
                int port = uri.getPort();
                String scheme = (uri.getScheme() == null) ? "http" : uri.getScheme();
                return scheme + "://" + host + (port > 0 ? ":" + port : "");
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    /* ───────────── 连不上时的地址设置页 ───────────── */

    private View buildOfflineView() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_VERTICAL);
        box.setBackgroundColor(Color.parseColor("#F0FDF4"));
        box.setPadding(dp(26), dp(26), dp(26), dp(26));
        box.setVisibility(View.GONE);

        TextView leaf = new TextView(this);
        leaf.setText("\uD83C\uDF3F");
        leaf.setTextSize(52);
        leaf.setGravity(Gravity.CENTER_HORIZONTAL);
        box.addView(leaf);

        final TextView title = new TextView(this);
        title.setText("连不上服务器");
        title.setTextSize(21);
        title.setTextColor(Color.parseColor("#14532D"));
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        box.addView(title, margin(0, 12, 0, 0));

        TextView hint = new TextView(this);
        hint.setText("请确认手机和服务器连在同一个 Wi-Fi 下，\n也可以在下面直接填写服务器地址。");
        hint.setTextSize(14);
        hint.setTextColor(Color.parseColor("#4B5563"));
        hint.setGravity(Gravity.CENTER_HORIZONTAL);
        box.addView(hint, margin(0, 10, 0, 0));

        offlineInput = new EditText(this);
        offlineInput.setSingleLine(true);
        offlineInput.setTextSize(15);
        offlineInput.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        offlineInput.setHint("http://YOUR_SERVER_HOST:8011/mobile/");
        offlineInput.setText(serverUrl(this));
        box.addView(offlineInput, margin(0, 22, 0, 0));

        Button save = new Button(this);
        save.setText("保存并重试");
        save.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String u = offlineInput.getText().toString().trim();
                if (u.length() == 0) {
                    Toast.makeText(MainActivity.this, "请先填写服务器地址", Toast.LENGTH_SHORT).show();
                    return;
                }
                saveUrl(u);
                hideOffline();
                load(serverUrl(MainActivity.this));
            }
        });
        box.addView(save, margin(0, 14, 0, 0));

        Button retry = new Button(this);
        retry.setText("仅重试一次");
        retry.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hideOffline();
                load(serverUrl(MainActivity.this));
            }
        });
        box.addView(retry, margin(0, 8, 0, 0));

        Button reset = new Button(this);
        reset.setText("恢复默认地址");
        reset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveUrl(DEFAULT_URL);
                offlineInput.setText(DEFAULT_URL);
                hideOffline();
                load(DEFAULT_URL);
            }
        });
        box.addView(reset, margin(0, 8, 0, 0));
        return box;
    }

    private LinearLayout.LayoutParams margin(int l, int t, int r, int b) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(l), dp(t), dp(r), dp(b));
        return lp;
    }

    private void showOffline(String why) {
        if (why != null && why.length() > 0) {
            ((TextView) ((LinearLayout) offlineView).getChildAt(1)).setText(why);
        }
        offlineView.setVisibility(View.VISIBLE);
        web.setVisibility(View.GONE);
    }

    private void hideOffline() {
        offlineView.setVisibility(View.GONE);
        web.setVisibility(View.VISIBLE);
    }

    /* ───────────── 返回键 ───────────── */

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (offlineView.getVisibility() == View.VISIBLE) {
                offlineView.setVisibility(View.GONE);
                web.setVisibility(View.VISIBLE);
                return true;
            }
            if (web.canGoBack()) {
                web.goBack();
                return true;
            }
            long now = System.currentTimeMillis();
            if (now - lastBackPress < 2000) {
                finish();
            } else {
                lastBackPress = now;
                Toast.makeText(this, "再按一次返回键退出", Toast.LENGTH_SHORT).show();
            }
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }
}
