require "UnLua"
require("LuaPanda").start("127.0.0.1",8818)

local UMG_Main_C = Class()

UMG_Main_C.ExitBtnText = "Hello World"

function UMG_Main_C:Construct()
    if not self.ExitButtonTextBlock then
        print("exit button text is nil!")
    else
        print("bind exit button text...")
        self.ExitButtonTextBlock:SetText(UMG_Main_C.ExitBtnText)
    end
    self.ExitButton.OnClicked:Add(self, UMG_Main_C.OnClicked_ExitButton)	
    --self.ExitButton.OnClicked:Add(self, function(Widget) UE4.UKismetSystemLibrary.ExecuteConsoleCommand(Widget, "exit") end )
end

function UMG_Main_C:OnClicked_ExitButton()
    UE4.UKismetSystemLibrary.ExecuteConsoleCommand(self, "exit")
end

return UMG_Main_C