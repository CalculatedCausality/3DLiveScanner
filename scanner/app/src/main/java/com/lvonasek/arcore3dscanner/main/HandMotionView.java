package com.lvonasek.arcore3dscanner.main;

import android.app.Activity;
import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.animation.Animation;
import android.view.animation.Transformation;
import android.widget.ImageView;

import com.lvonasek.arcore3dscanner.R;


/** This view contains the hand motion instructions with animation. */

public class HandMotionView extends ImageView {

  private static final long ANIMATION_SPEED_MS = 2500;
  private static final float TWO_PI = (float) Math.PI * 2.0f;
  private static final float HALF_PI = (float) Math.PI / 2.0f;

  public HandMotionView(Context context) {
    super(context);
  }

  public HandMotionView(Context context, AttributeSet attrs) {
    super(context, attrs);
  }

  @Override
  protected void onAttachedToWindow() {
    super.onAttachedToWindow();

    clearAnimation();

    View container = ((Activity) getContext()).findViewById(R.id.ar_hand_layout);
    Animation animation = new Animation() {
      @Override
      protected void applyTransformation(float interpolatedTime, Transformation transformation) {
        float progressAngle = TWO_PI * interpolatedTime;
        float currentAngle = HALF_PI + progressAngle;

        float handWidth = getWidth();
        float radius = getResources().getDisplayMetrics().density * 25.0f;

        float xPos = radius * 2.0f * (float) Math.cos(currentAngle);
        float yPos = radius * (float) Math.sin(currentAngle);

        xPos += container.getWidth() / 2.0f;
        yPos += container.getHeight() / 2.0f;

        xPos -= handWidth / 2.0f;
        yPos -= getHeight() / 2.0f;

        // Position the hand.
        setX(xPos);
        setY(yPos);

        invalidate();
      }
    };
    animation.setRepeatCount(Animation.INFINITE);
    animation.setDuration(ANIMATION_SPEED_MS);
    animation.setStartOffset(1000);
    startAnimation(animation);
  }
}
