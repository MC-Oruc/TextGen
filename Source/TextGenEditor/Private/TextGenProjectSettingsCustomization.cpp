#include "TextGenProjectSettingsCustomization.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Modules/ModuleManager.h"
#include "TextGenEditorModule.h"
#include "TextGen/TextGenProjectSettings.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TextGenProjectSettingsCustomization"

TSharedRef<IDetailCustomization> FTextGenProjectSettingsCustomization::MakeInstance()
{
    return MakeShared<FTextGenProjectSettingsCustomization>();
}

void FTextGenProjectSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Runtime Installation"),
        LOCTEXT("RuntimeInstallationCategory", "Runtime Installation"), ECategoryPriority::Important);
    Category.AddCustomRow(LOCTEXT("PrepareRuntimesFilter", "Prepare llama.cpp runtimes"))
    .WholeRowContent()
    [
        SNew(SUniformGridPanel)
        .SlotPadding(FMargin(4.0f))
        + SUniformGridPanel::Slot(0, 0)
        [
            SNew(SButton)
            .Text(LOCTEXT("PrepareDevelopmentRuntimes", "Prepare Development Runtimes"))
            .ToolTipText(LOCTEXT("PrepareDevelopmentRuntimesTooltip",
                "Install CUDA 13, CUDA 12, and Vulkan runtimes for Development Defaults without starting PIE."))
            .OnClicked(this, &FTextGenProjectSettingsCustomization::PrepareDevelopmentRuntimes)
        ]
        + SUniformGridPanel::Slot(1, 0)
        [
            SNew(SButton)
            .Text(LOCTEXT("PreparePackagedRuntimes", "Prepare Packaged Runtimes"))
            .ToolTipText(LOCTEXT("PreparePackagedRuntimesTooltip",
                "Install CUDA 13, CUDA 12, and Vulkan runtimes for Packaged Game Defaults without starting PIE."))
            .OnClicked(this, &FTextGenProjectSettingsCustomization::PreparePackagedRuntimes)
        ]
    ];
}

FReply FTextGenProjectSettingsCustomization::PrepareDevelopmentRuntimes() const
{
    FTextGenEditorModule& Module = FModuleManager::LoadModuleChecked<FTextGenEditorModule>(TEXT("TextGenEditor"));
    Module.PrepareRuntimesForTag(GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults.RuntimeTag);
    return FReply::Handled();
}

FReply FTextGenProjectSettingsCustomization::PreparePackagedRuntimes() const
{
    FTextGenEditorModule& Module = FModuleManager::LoadModuleChecked<FTextGenEditorModule>(TEXT("TextGenEditor"));
    Module.PrepareRuntimesForTag(GetDefault<UTextGenProjectSettings>()->PackagedDefaults.RuntimeTag);
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
