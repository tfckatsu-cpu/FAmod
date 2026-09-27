module;
#include <imgui.h>

module patch.share_factory_assist;
import core;

void ShareFactoryAssistPatch::RenderUi() {
  ImGui::Checkbox(tr("Factory Assist Orders",
                     {{Language::Russian, "Приказы ассиста фабрик"},
                      {Language::Chinese, "工厂协助指令"}}),
                  &enabled_);

  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
    ImGui::TextUnformatted(
        tr("Allows factories to issue assist orders for produced units (target "
           "or ground rally). Assisting another factory as the first command "
           "copies its build queue, while queuing with Shift sets produced "
           "units "
           "to assist it.",
           {{Language::Russian,
             "Позволяет фабрикам отдавать приказ ассиста для произведённых "
             "юнитов "
             "(на цель или точку на земле). Первый приказ на другую фабрику "
             "копирует её очередь постройки, а через Shift — задаёт ассист "
             "произведёнными юнитами."},
            {Language::Chinese,
             "允许工厂为其生产的单位下达协助指令（目标或地面集结点）。"
             "对另一座工厂的首个指令将复制其建造队列，通过 Shift "
             "添加则让生产出的单位协助该工厂。"}})
            .c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  if (enabled_) {
    ImGui::Indent();
    ImGui::Checkbox(tr("Disable queue copying (first order)",
                       {{Language::Russian,
                         "Отключить копирование очереди (первый приказ)"},
                        {Language::Chinese, "禁用队列复制（首个指令）"}}),
                    &disable_queue_copy_);

    if (ImGui::BeginItemTooltip()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
      ImGui::TextUnformatted(
          tr("When enabled, assisting another factory as the first command "
             "sets produced units to assist it instead of copying its build "
             "queue.",
             {{Language::Russian,
               "Если включено, первый приказ ассиста на другую фабрику задаёт "
               "ассист произведёнными юнитами вместо копирования её очереди "
               "постройки."},
              {Language::Chinese, "启用后，对另一座工厂的首个协助指令将直接让生"
                                  "产出的单位协助该工厂，"
                                  "而不会复制其建造队列。"}})
              .c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    ImGui::Unindent();
  }
}
