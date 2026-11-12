// Append as an AVM1 DoAction after this movie's ButtonPrompt class definition.
// The native frame update publishes the same mode as the font keycaps.
if (!_root.__rccitPromptDriver && typeof ButtonPrompt == "function")
{
   _root.__rccitPrompts = new Array();
   _root.__rccitAddPrompt = function(prompt)
   {
      if (prompt.__rccitKeycap) return;
      var size = prompt.buttonIconDimensions;
      prompt.createTextField("__rccitKeycap",prompt.getNextHighestDepth(),0,-2,size,size + 4);
      prompt.__rccitKeycap.background = true;
      prompt.__rccitKeycap.backgroundColor = 0x101820;
      prompt.__rccitKeycap.border = true;
      prompt.__rccitKeycap.borderColor = 0xFFFFFF;
      prompt.__rccitKeycap.selectable = false;
      prompt.__rccitKeycap.multiline = false;
      prompt.__rccitKeycap.wordWrap = false;
      prompt.__rccitKeycap._visible = false;
      _root.__rccitPrompts.push(prompt);
   };
   ButtonPrompt.prototype.__rccitCreateIcon = ButtonPrompt.prototype.CreateIcon;
   ButtonPrompt.prototype.CreateIcon = function()
   {
      this.__rccitCreateIcon();
      _root.__rccitAddPrompt(this);
   };
   // Existing first-frame instances may precede this appended action.
   _root.__rccitFindPrompts = function(parent)
   {
      for (var name in parent)
      {
         var child = parent[name];
         if (typeof child == "movieclip" && child._parent == parent)
         {
            if (child instanceof ButtonPrompt) _root.__rccitAddPrompt(child);
            _root.__rccitFindPrompts(child);
         }
      }
   };
   _root.createEmptyMovieClip("__rccitPromptDriver",_root.getNextHighestDepth());
   _root.__rccitPromptDriver.onEnterFrame = function()
   {
      var keyboard = _root.__rccit_mode == "keyboard";
      for (var i = _root.__rccitPrompts.length - 1; i >= 0; --i)
      {
         var prompt = _root.__rccitPrompts[i];
         if (!prompt._parent)
         {
            _root.__rccitPrompts.splice(i,1);
            continue;
         }
         var type = prompt.buttonType;
         if (type == "enter") type = "cross";
         if (type == "back") type = "circle";
         if (type == "ljoystick") type = "lstick";
         if (type == "rjoystick") type = "rstick";
         var label = _root["__rccit_" + type];
         var showKey = keyboard && label != undefined && label.length > 0;
         if (showKey && !prompt.__rccitShowingKey)
         {
            prompt.__rccitIconVisible = prompt.btn_icon._visible;
            prompt.__rccitGlowVisible = prompt.buttonGlow._visible;
         }
         if (showKey)
         {
            prompt.btn_icon._visible = false;
            if (prompt.buttonGlow) prompt.buttonGlow._visible = false;
            if (prompt.__rccitKeycap.text != label)
            {
               prompt.__rccitKeycap.text = label;
               var size = 16;
               GFuncs.TextFormatting(prompt.__rccitKeycap,size,prompt.buttonPromptFormat,"center",false);
               var format = prompt.__rccitKeycap.getTextFormat();
               while (prompt.__rccitKeycap.textWidth > prompt.buttonIconDimensions - 4 && size > 1)
               {
                  format.size = --size;
                  prompt.__rccitKeycap.setTextFormat(format);
               }
               prompt.__rccitKeycap.textColor = 0xFFFFFF;
            }
         }
         else if (prompt.__rccitShowingKey)
         {
            prompt.btn_icon._visible = prompt.__rccitIconVisible;
            if (prompt.buttonGlow) prompt.buttonGlow._visible = prompt.__rccitGlowVisible;
         }
         prompt.__rccitKeycap._visible = showKey;
         prompt.__rccitShowingKey = showKey;
      }
      var title = _root.mainMenu.startPrompt.title;
      if (title)
      {
         if (title.__rccitOriginalText == undefined)
         {
            title.__rccitOriginalText = title.text;
            title.__rccitOriginalHTML = title.htmlText;
            title.__rccitOriginalHTMLMode = title.html;
            title.__rccitOriginalFormat = title.getTextFormat();
            title.__rccitOriginalNewFormat = title.getNewTextFormat();
         }
         if (keyboard && _root.__rccit_start != undefined)
         {
            var label = "PRESS " + _root.__rccit_start;
            if (!title.__rccitShowingKey || title.__rccitKeyboardLabel != label)
            {
               title.text = label;
               title.setTextFormat(title.__rccitOriginalFormat);
               title.setNewTextFormat(title.__rccitOriginalNewFormat);
               title.__rccitKeyboardLabel = label;
            }
            title.__rccitShowingKey = true;
         }
         else if (title.__rccitShowingKey)
         {
            if (title.__rccitOriginalHTMLMode) title.htmlText = title.__rccitOriginalHTML;
            else title.text = title.__rccitOriginalText;
            title.setTextFormat(title.__rccitOriginalFormat);
            title.setNewTextFormat(title.__rccitOriginalNewFormat);
            title.__rccitShowingKey = false;
         }
      }
   };
   _root.__rccitFindPrompts(_root);
   delete _root.__rccitFindPrompts;
}
