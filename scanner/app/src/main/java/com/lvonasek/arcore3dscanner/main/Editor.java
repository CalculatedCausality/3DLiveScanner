package com.lvonasek.arcore3dscanner.main;

import androidx.appcompat.app.AlertDialog;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Point;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import com.lvonasek.arcore3dscanner.ui.AbstractActivity;
import com.lvonasek.arcore3dscanner.R;
import com.lvonasek.utils.IO;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;

public class Editor extends View implements Button.OnClickListener, View.OnTouchListener {

  private final int BUTTON_SAVE = 0;
  private final int BUTTON_SUBMENU_SELECT = 5;
  private final int BUTTON_SUBMENU_COLORS = 11;
  private final int BUTTON_SUBMENU_TRANSFORM = 16;
  private final int BUTTON_SUBMENU_VIEW = 19;
  private final int BUTTON_X = 23;
  private final int BUTTON_Y = 24;
  private final int BUTTON_Z = 25;

  private enum Effect { CONTRAST, GAMMA, SATURATION, TONE, RESET, CLONE, DELETE, MOVE, ROTATE, SCALE }
  private enum Screen { MAIN, COLOR, SELECT, TRANSFORM, EDIT}
  private enum Status { IDLE, SELECT_OBJECT, SELECT_CIRCLE, SELECT_RECT, UPDATE_COLORS, UPDATE_TRANSFORM }

  private int mAxis;
  private ArrayList<Button> mButtons;
  private AbstractActivity mContext;
  private Effect mEffect;
  private ProgressBar mProgress;
  private Screen mScreen;
  private SeekBar mSeek;
  private Status mStatus;
  private TextView mMsg;
  private Paint mPaint;
  private Rect mRect;
  private CheckBox mDeselect;
  private Point mCircleCenter;
  private float mCircleRadius;
  private boolean mShowNormals;

  private boolean mBackShown;
  private boolean mComplete;
  private boolean mInitialized;
  private boolean mSaveInProgress;

  public Editor(Context context, AttributeSet attrs)
  {
    super(context, attrs);
    mInitialized = false;
    mPaint = new Paint();
    mPaint.setColor(0x8080FF80);
    mRect = new Rect();
    mCircleCenter = new Point();
    mCircleRadius = 0;
    mShowNormals = false;
  }

  public void init(ArrayList<Button> buttons, TextView msg, SeekBar seek, ProgressBar progress, CheckBox deselect, AbstractActivity context)
  {
    for (Button b : buttons) {
      b.setOnClickListener(this);
      b.setOnTouchListener(this);
    }
    mAxis = 1;
    mButtons = buttons;
    mContext = context;
    mDeselect = deselect;
    mMsg = msg;
    mProgress = progress;
    mSeek = seek;
    setScreen(Screen.MAIN, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_SELECT);

    mComplete = true;
    mInitialized = true;
    mProgress.setVisibility(View.VISIBLE);
    mSeek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener()
    {
      @Override
      public void onProgressChanged(SeekBar seekBar, int value, boolean byUser)
      {
        if ((mStatus == Status.UPDATE_COLORS) || (mStatus == Status.UPDATE_TRANSFORM)) {
          value -= 127;
          JNI.previewEffect(mEffect.ordinal(), value, mAxis);
        }
      }

      @Override
      public void onStartTrackingTouch(SeekBar seekBar)
      {
      }

      @Override
      public void onStopTrackingTouch(SeekBar seekBar)
      {
      }
    });
    new Thread(() -> {
      JNI.completeSelection(mComplete);
      mContext.runOnUiThread(() -> mProgress.setVisibility(View.GONE));
    }).start();
  }

  private void applyTransform()
  {
    final int axis = mAxis;
    mProgress.setVisibility(View.VISIBLE);
    new Thread(() -> {
      JNI.applyEffect(mEffect.ordinal(), mSeek.getProgress() - 127, axis);
      mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
    }).start();
  }

  public boolean initialized() { return mInitialized; }

  public boolean movingLocked()
  {
    return mStatus != Status.IDLE;
  }

  private Rect normalizeRect(Rect input)
  {
    return new Rect(Math.min(input.left, input.right), Math.min(input.top, input.bottom),
        Math.max(input.left, input.right), Math.max(input.top, input.bottom));
  }

  @Override
  public void onClick(final View view)
  {
    //axis buttons
    if (view.getId() == R.id.editorX) {
      applyTransform();
      mAxis = 0;
      showSeekBar(true);
    }
    if (view.getId() == R.id.editorY) {
      applyTransform();
      mAxis = 1;
      showSeekBar(true);
    }
    if (view.getId() == R.id.editorZ) {
      applyTransform();
      mAxis = 2;
      showSeekBar(true);
    }

    //back button
    if (view.getId() == R.id.editor0) {
      if ((mStatus == Status.SELECT_OBJECT) || (mStatus == Status.SELECT_CIRCLE) || (mStatus == Status.SELECT_RECT)) {
        mDeselect.setVisibility(View.GONE);
        setScreen(Screen.MAIN, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_SELECT);
      } else if (mStatus == Status.UPDATE_COLORS) {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
            JNI.applyEffect(mEffect.ordinal(), mSeek.getProgress() - 127, 0);
            mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
          }).start();
        setScreen(Screen.MAIN, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_SELECT);
      } else if (mStatus == Status.UPDATE_TRANSFORM) {
        applyTransform();
        setScreen(Screen.MAIN, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_SELECT);
      } else
        save();
    }
    //main menu
    else if (view.getId() == R.id.editor1)
      setScreen(Screen.SELECT, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_COLORS);
    else if (view.getId() == R.id.editor2)
      setScreen(Screen.COLOR, BUTTON_SUBMENU_COLORS, BUTTON_SUBMENU_TRANSFORM);
    else if (view.getId() == R.id.editor3)
      setScreen(Screen.TRANSFORM, BUTTON_SUBMENU_TRANSFORM, BUTTON_SUBMENU_VIEW);
    else if (view.getId() == R.id.editor4)
      setScreen(Screen.EDIT, BUTTON_SUBMENU_VIEW, BUTTON_X);

    //selecting objects
    if (mScreen == Screen.SELECT) {
      //select all/none
      if (view.getId() == R.id.editor1a)
      {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          mComplete = !mComplete;
          JNI.completeSelection(mComplete);
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.GONE));
        }).start();
      }
      //select object
      if (view.getId() == R.id.editor1b) {
        showText(R.string.editor_select_object_desc);
        mBackShown = true;
        mButtons.get(BUTTON_SAVE).setBackgroundResource(R.drawable.ic_back_small);
        mStatus = Status.SELECT_OBJECT;
      }
      //rect selection
      if (view.getId() == R.id.editor1c) {
        showText(R.string.editor_select_circle_desc);
        mBackShown = true;
        mButtons.get(BUTTON_SAVE).setBackgroundResource(R.drawable.ic_back_small);
        mDeselect.setVisibility(View.VISIBLE);
        mStatus = Status.SELECT_CIRCLE;
      }
      //rect selection
      if (view.getId() == R.id.editor1d) {
        showText(R.string.editor_select_rect_desc);
        mBackShown = true;
        mButtons.get(BUTTON_SAVE).setBackgroundResource(R.drawable.ic_back_small);
        mDeselect.setVisibility(View.VISIBLE);
        mStatus = Status.SELECT_RECT;
      }
      //select less/more
      if (view.getId() == R.id.editor1e || view.getId() == R.id.editor1f)
      {
        final boolean more = view.getId() == R.id.editor1f;
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          JNI.multSelection(more);
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.GONE));
        }).start();
      }
    }

    //color editing
    if (mScreen == Screen.COLOR) {
      if (view.getId() != R.id.editor0 && mShowNormals) {
        mShowNormals = false;
        JNI.showNormals(false);
      }

      if (view.getId() == R.id.editor2a)
        startEffect(Effect.CONTRAST, Status.UPDATE_COLORS, false);
      if (view.getId() == R.id.editor2b)
        startEffect(Effect.GAMMA, Status.UPDATE_COLORS, false);
      if (view.getId() == R.id.editor2c)
        startEffect(Effect.SATURATION, Status.UPDATE_COLORS, false);
      if (view.getId() == R.id.editor2d)
        startEffect(Effect.TONE, Status.UPDATE_COLORS, false);
      if (view.getId() == R.id.editor2e)
      {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          JNI.applyEffect(Effect.RESET.ordinal(), 0, 0);
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
        }).start();
      }
    }

    // transforming objects
    if (mScreen == Screen.TRANSFORM) {
      if (view.getId() == R.id.editor3a)
        startEffect(Effect.MOVE, Status.UPDATE_TRANSFORM, true);
      if (view.getId() == R.id.editor3b)
        startEffect(Effect.ROTATE, Status.UPDATE_TRANSFORM, true);
      if (view.getId() == R.id.editor3c)
        startEffect(Effect.SCALE, Status.UPDATE_TRANSFORM, false);
    }

    //view
    if (mScreen == Screen.EDIT) {
      if (view.getId() == R.id.editor4a)
      {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          JNI.restore();
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
        }).start();
      }
      if (view.getId() == R.id.editor4b)
      {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          JNI.applyEffect(Effect.CLONE.ordinal(), 0, 0);
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
        }).start();
      }
      if (view.getId() == R.id.editor4c)
      {
        mProgress.setVisibility(View.VISIBLE);
        new Thread(() -> {
          JNI.applyEffect(Effect.DELETE.ordinal(), 0, 0);
          mContext.runOnUiThread(() -> mProgress.setVisibility(View.INVISIBLE));
        }).start();
      }
      if (view.getId() == R.id.editor4d)
      {
        swapNormals();
      }
    }
  }

  @Override
  protected void onDraw(Canvas c)
  {
    super.onDraw(c);
    c.drawCircle(mCircleCenter.x, mCircleCenter.y, mCircleRadius, mPaint);
    c.drawRect(normalizeRect(mRect), mPaint);
  }

  @Override
  public boolean onTouch(View view, MotionEvent motionEvent)
  {
    if (view instanceof Button) {
      Button b = (Button) view;
      if (motionEvent.getAction() == MotionEvent.ACTION_DOWN)
        b.setTextColor(Color.YELLOW);
      if (motionEvent.getAction() == MotionEvent.ACTION_UP)
        b.setTextColor(Color.WHITE);
    }
    return false;
  }

  private void setScreen(Screen screen, int firstButton, int endButton)
  {
    mMsg.setVisibility(View.GONE);
    mSeek.setVisibility(View.GONE);
    mStatus = Status.IDLE;
    for (Button b : mButtons) {
      b.setVisibility(View.VISIBLE);
    }
    mButtons.get(BUTTON_X).setVisibility(View.GONE);
    mButtons.get(BUTTON_Y).setVisibility(View.GONE);
    mButtons.get(BUTTON_Z).setVisibility(View.GONE);
    mButtons.get(BUTTON_SAVE).setBackgroundResource(R.drawable.ic_save_small);
    mBackShown = false;
    for (int i = BUTTON_SUBMENU_SELECT; i < mButtons.size(); i++) {
      mButtons.get(i).setVisibility(i >= firstButton && i < endButton ? View.VISIBLE : View.GONE);
    }
    mScreen = screen;
  }

  private void save() {
    if (mSaveInProgress) return;
    //filename dialog
    AlertDialog.Builder builder = new MaterialAlertDialogBuilder(mContext);
    builder.setTitle(mContext.getString(R.string.enter_filename));
    final EditText input = new EditText(mContext);
    builder.setView(input);
    builder.setPositiveButton(mContext.getString(android.R.string.ok), (dialog, which) -> {
      if (mSaveInProgress) return;
      final String filename = input.getText().toString().trim();
      mSaveInProgress = true;
      mProgress.setVisibility(View.VISIBLE);
      new Thread(() -> {
        File staging = null;
        boolean saved = false;
        try {
          if (filename.isEmpty()) throw new IOException("Missing model name");
          File destination = IO.resolveContainedFile(new File(AbstractActivity.getPath(false)), filename + Exporter.EXT_OBJ);
          if (destination.exists()) throw new IOException("Scan already exists");
          staging = IO.createStagingDirectory(AbstractActivity.getScratchPath());
          File obj = new File(staging, "edited.obj");
          if (!JNI.saveWithTextures(obj.getAbsolutePath().getBytes())) throw new IOException("Native model write failed");
          Exporter.export(obj, filename);
          saved = true;
        } catch (Exception failure) {
          Log.e(AbstractActivity.TAG, "Unable to save edited model; existing scans retained", failure);
        } finally {
          if (staging != null) IO.deleteRecursive(staging);
          final boolean success = saved;
          mContext.runOnUiThread(() -> {
            mSaveInProgress = false;
            mProgress.setVisibility(View.GONE);
            if (!mContext.isFinishing() && !mContext.isDestroyed()) {
              Toast.makeText(mContext, success ? R.string.data_saved : R.string.storage_editor_failed, Toast.LENGTH_LONG).show();
            }
          });
        }
      }).start();
      dialog.cancel();
    });
    builder.setNegativeButton(mContext.getString(android.R.string.cancel), null);
    builder.create().show();
  }

  private void startEffect(Effect effect, Status status, boolean axes)
  {
    mEffect = effect;
    mStatus = status;
    mBackShown = true;
    mButtons.get(BUTTON_SAVE).setBackgroundResource(R.drawable.ic_back_small);
    showSeekBar(axes);
  }

  private void showSeekBar(boolean axes)
  {
    for (Button b : mButtons)
      b.setVisibility(View.GONE);
    mButtons.get(BUTTON_SAVE).setVisibility(View.VISIBLE);
    if (axes)
      updateAxisButtons();
    mSeek.setMax(255);
    mSeek.setProgress(127);
    mSeek.setVisibility(View.VISIBLE);
  }

  private void showText(int resId)
  {
    for (Button b : mButtons)
      b.setVisibility(View.GONE);
    mButtons.get(BUTTON_SAVE).setVisibility(View.VISIBLE);
    mMsg.setText(mContext.getString(resId));
    mMsg.setVisibility(View.VISIBLE);
  }

  public void swapNormals()
  {
    mShowNormals = !mShowNormals;
    JNI.showNormals(mShowNormals);
  }

  public void touchEvent(final MotionEvent event)
  {
    if (!mBackShown) {
      setScreen(Screen.MAIN, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_SELECT);
      return;
    }

    if (mStatus == Status.SELECT_OBJECT) {
      mProgress.setVisibility(View.VISIBLE);
      new Thread(() -> {
        JNI.applySelect(event.getX(), getHeight() - event.getY(), false);
        mContext.runOnUiThread(() -> mProgress.setVisibility(View.GONE));
      }).start();
      mStatus = Status.IDLE;
      setScreen(Screen.SELECT, BUTTON_SUBMENU_SELECT, BUTTON_SUBMENU_COLORS);
    }

    if (mStatus == Status.SELECT_CIRCLE) {
      if (event.getAction() == MotionEvent.ACTION_DOWN) {
        mCircleCenter.x = (int) event.getX();
        mCircleCenter.y = (int) event.getY();
        mCircleRadius = 0;
      }
      if (event.getAction() == MotionEvent.ACTION_MOVE) {
        float dx = mCircleCenter.x - (int) event.getX();
        float dy = mCircleCenter.y - (int) event.getY();
        mCircleRadius = (float) Math.sqrt(dx * dx + dy * dy);
      }
      if (event.getAction() == MotionEvent.ACTION_UP) {
        float dx = mCircleCenter.x - (int) event.getX();
        float dy = mCircleCenter.y - (int) event.getY();
        mCircleCenter.y = getHeight() - mCircleCenter.y;
        mCircleRadius = (float) Math.sqrt(dx * dx + dy * dy);
        JNI.circleSelection(mCircleCenter.x, mCircleCenter.y, mCircleRadius, mDeselect.isChecked());
        mCircleCenter = new Point();
        mCircleRadius = 0;
      }
      postInvalidate();
    }

    if (mStatus == Status.SELECT_RECT) {
      if (event.getAction() == MotionEvent.ACTION_DOWN) {
        mRect.left = (int) event.getX();
        mRect.top = (int) event.getY();
        mRect.right = mRect.left;
        mRect.bottom = mRect.top;
      }
      if (event.getAction() == MotionEvent.ACTION_MOVE) {
        mRect.right = (int) event.getX();
        mRect.bottom = (int) event.getY();
      }
      if (event.getAction() == MotionEvent.ACTION_UP) {
        Rect rect = normalizeRect(mRect);
        rect.top = getHeight() - rect.top;
        rect.bottom = getHeight() - rect.bottom;
        JNI.rectSelection(rect.left, rect.bottom, rect.right, rect.top, mDeselect.isChecked());
        mRect = new Rect();
      }
      postInvalidate();
    }
  }

  private void updateAxisButtons()
  {
    mButtons.get(BUTTON_X).setVisibility(View.VISIBLE);
    mButtons.get(BUTTON_Y).setVisibility(View.VISIBLE);
    mButtons.get(BUTTON_Z).setVisibility(View.VISIBLE);
    mButtons.get(BUTTON_X).setText("X");
    mButtons.get(BUTTON_Y).setText("Y");
    mButtons.get(BUTTON_Z).setText("Z");
    mButtons.get(BUTTON_X).setTextColor(mAxis == 0 ? Color.BLACK : Color.WHITE);
    mButtons.get(BUTTON_Y).setTextColor(mAxis == 1 ? Color.BLACK : Color.WHITE);
    mButtons.get(BUTTON_Z).setTextColor(mAxis == 2 ? Color.BLACK : Color.WHITE);
    mButtons.get(BUTTON_X).setBackgroundColor(mAxis == 0 ? Color.WHITE : Color.TRANSPARENT);
    mButtons.get(BUTTON_Y).setBackgroundColor(mAxis == 1 ? Color.WHITE : Color.TRANSPARENT);
    mButtons.get(BUTTON_Z).setBackgroundColor(mAxis == 2 ? Color.WHITE : Color.TRANSPARENT);
  }
}
