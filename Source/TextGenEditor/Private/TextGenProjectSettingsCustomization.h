#pragma once

#include "IDetailCustomization.h"

class FTextGenProjectSettingsCustomization final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();
    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
    FReply PrepareDevelopmentRuntimes() const;
    FReply PreparePackagedRuntimes() const;
};
