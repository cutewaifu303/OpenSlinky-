public class SlinkyHooks13 {
    public static native void getMouseOver(Object entityRenderer, float partialTicks);

    public static native boolean isDisplayActivePreRender();

    public static native void updateDisplay();

    public static native void postWorldClear_1_7(int mask);

    public static native void postWorldClear_1_8(int mask);

    public static native void clickMouse(Object minecraft);

    public static native void rightClickMouse(Object minecraft);

    public static native void orientCamera(Object entityRenderer, float partialTicks);

    public static native void updateActiveRenderInfo(Object player, boolean thirdPersonView);

    public static native void setRenderX(Object entityRenderer, float angleX);

    public static native void setRenderY(Object entityRenderer, float angleY);

    public static native void updateEntities(Object world);

    public static native boolean canRenderName(Object renderer, Object entity);

    public static native void renderPlayer(
        Object renderPlayer,
        Object clientPlayer,
        double x,
        double y,
        double z,
        float entityYaw,
        float partialTicks
    );

    public static native void unloadChunk(Object chunkProvider, int chunkX, int chunkZ);

    public static native boolean setBlock_1_7(
        Object world,
        int x,
        int y,
        int z,
        Object block,
        int metadata,
        int flags
    );

    public static native boolean setBlock_1_8(
        Object world,
        Object blockPos,
        Object blockState,
        int flags
    );

    public static native void invalidateChunk(
        Object world,
        int minX,
        int minY,
        int minZ,
        int maxX,
        int maxY,
        int maxZ
    );

    public static native boolean shouldClipLedge(Object entity);

    public static native void moveFlying(Object entity, float strafe, float forward, float friction);

    public static native void sendPacket_1_7(Object netHandler, Object packet, Object listeners);

    public static native void sendPacket_1_8(Object netHandler, Object packet);

    public static native void updateController(Object playerController);

    // ---- OpenSlinky++ extra hooks (Adjust visual style, see src/adjust/hooks) ----

    public static native boolean adjustShowCrosshair(Object guiIngame);

    public static native void adjustDrawSelectionBoundingBox(Object aabb);

    public static native void adjustRenderModel(Object renderer, Object entity, float limbSwing, float limbSwingAmount,
                                                float ageInTicks, float headYaw, float headPitch, float scale);

    public static native void adjustHurtcamHurtCameraEffect(Object entityRenderer, float partialTicks);

    public static native void adjustHurtcamTexEnv(int target, int pname, Object buffer);

    public static native void adjustHurtcamTexEnvfv(int target, int pname, Object buffer);

    public static native void adjustHurtcamShadersEntityColor(float r, float g, float b, float a);

    public static native void adjustHurtcamRenderArmorLayer(Object layer, Object entity, float limbSwing, float limbSwingAmount, float partialTicks, float ageInTicks, float netHeadYaw, float headPitch, float scale, int armorSlot);

    public static native void adjustStreamerSetChatLine(Object guiNewChat, Object component, int chatLineId, int updateCounter, boolean displayOnly);

    public static native java.util.Collection adjustStreamerSortedScores(Object scoreboard, Object objective);

    public static native String adjustStreamerTabPlayerName(Object tabOverlay, Object networkPlayerInfo);

    public static native void adjustStreamerRenderLivingLabel(Object render, Object entity, Object str, double x, double y, double z, int maxDistance);

    public static native double adjustStreamerNameTagDistance(Object entity, Object other);

    public static native int adjustStreamerNameTagWidth(Object fontRenderer, Object text);

    public static native int adjustStreamerNameTagDraw(Object fontRenderer, Object text, int x, int y, int color);

    public static native void adjustStreamerNameTagOffsetLabel(Object renderer, Object entity, double x, double y, double z, Object text, float scale, double distanceSq);

    public static native java.util.List adjustStreamerSignSplitText(Object component, int width, Object fontRenderer, boolean keepNewlines, boolean forceTextColor);

    public static native void adjustStreamerSendChatMessage(Object guiScreen, Object message);

    public static native void adjustMenuDrawScreen(Object screen, int mouseX, int mouseY, float partialTicks);

    public static native void adjustMenuForgeDrawScreen(Object screen, int mouseX, int mouseY, float partialTicks);

    public static native void adjustMenuHandleInput(Object screen);

    public static native int adjustMenuGetLimitFramerate(Object minecraft);

    public static native float adjustItemSwingProgress(Object player, float partialTicks);

    public static native void adjustItemFirstPersonPushMatrix();

    public static native void adjustItemTransformFirstPerson(Object itemRenderer, float equipProgress, float swingProgress);

    public static native void adjustItemBlockTransformations(Object itemRenderer);

    public static native void adjustItemRenderFirstPerson(Object itemRenderer, Object entity, Object stack, Object transform);

    public static native void adjustItemBowScale(float x, float y, float z);

    public static native void adjustItemHeldItemRender(Object itemRenderer, Object entity, Object stack, Object transform);

    public static native void adjustItemModelTransformRender(Object renderItem, Object stack, Object model);

    public static native void adjustItemGlintRenderModel(Object renderItem, Object model, int color);

    public static native void adjustItemBipedRotateAngleY(Object modelRenderer, float value);

    public static native int adjustItemArmSwingAnimationEnd(Object living);

    public static native void adjustItemPhysicsDoRender(Object renderer, Object entityItem, double x, double y, double z, float entityYaw, float partialTicks);

    public static native void adjustWorldFxRenderWorld(Object entityRenderer, float partialTicks, long finishTimeNano);

    public static native void adjustWorldFxSetupCameraTransform(Object entityRenderer, float partialTicks, int pass);

    public static native boolean adjustWorldFxRenderEntitySimple(Object renderManager, Object entity, float partialTicks);

    public static native float adjustWorldFxGamma(Object gameSettings);

    public static native boolean adjustWorldFxIsPotionActive(Object entity, Object potion);

    public static native void adjustWorldFxUpdateLightmapTexture(Object dynamicTexture);

    public static native boolean adjustWorldFxIsCustomColors();

    public static native void adjustWorldFxFogClearColor(float red, float green, float blue, float alpha);

    public static native void adjustWorldFxFogClearColorShaders(float red, float green, float blue, float alpha);

    public static native void adjustWorldFxSetupFogEnd();

    public static native long adjustWorldFxWorldInfoTime(Object worldInfo);

    public static native long adjustWorldFxWorldTime(Object world);

    public static native double adjustCameraViewClipDistance(Object vec, Object other);

    public static native void adjustCameraSetAngles(Object entity, float yaw, float pitch);

    public static native void adjustCameraRenderHand(Object entityRenderer, float partialTicks, int pass);

    public static native boolean adjustCameraMovementKeyDown(Object keyBinding);

    public static native boolean adjustCameraIsCurrentViewEntity(Object player);

    public static native Object adjustCameraHudViewEntity(Object minecraft);

    public static native int adjustTweaksTitlesTimer(Object guiIngame);

    public static native void adjustTweaksRenderBossHealth(Object guiIngame);

    public static native void adjustTweaksUpdateAchievementWindow(Object guiAchievement);

    public static native void adjustTweaksRenderScoreboard(Object guiIngame, Object objective, Object scaledResolution);

    public static native void adjustTweaksScoreboardDrawRect(int left, int top, int right, int bottom, int color);

    public static native int adjustTweaksScoreboardDrawString(Object fontRenderer, Object text, int x, int y, int color);

    public static native int adjustTweaksScoreboardStringWidth(Object fontRenderer, Object text);

    public static native void adjustTweaksChatDrawRect(int left, int top, int right, int bottom, int color);

    public static native int adjustTweaksChatLineCounter(Object chatLine);

    public static native int adjustTweaksChatDrawString(Object fontRenderer, Object text, float x, float y, int color);

    public static native java.util.List adjustTweaksChatSplitText(Object component, int width, Object fontRenderer, boolean styled, boolean trim);

    public static native void adjustTweaksChangeCurrentItem(Object inventory, int direction);

    public static native float adjustTweaksGetFovModifier(Object entityRenderer, float partialTicks, boolean useFovSetting);

    public static native void adjustTweaksSetFullscreen(boolean fullscreen);

    public static native void adjustTweaksActionPerformed(Object guiScreen, Object button);

    public static native java.util.Collection adjustTweaksTabPlayerInfoMap(Object netHandler);

    public static native void adjustTweaksOtherPlayerSwing(Object entity);

    public static native void adjustTweaksSetRotationYawHead(Object entity, float yaw);

    public static native void adjustTweaksSetupViewBobbing(Object entityRenderer, float partialTicks);
}
