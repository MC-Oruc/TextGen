#pragma once

#include "ContentBrowserFileDataSource.h"
#include "TextGenContentBrowserDataSource.generated.h"

UCLASS()
class TEXTGENEDITOR_API UTextGenContentBrowserDataSource final : public UContentBrowserFileDataSource
{
    GENERATED_BODY()

public:
    virtual bool DeleteItem(const FContentBrowserItemData& InItem) override;
    virtual bool BulkDeleteItems(TArrayView<const FContentBrowserItemData> InItems) override;
};
