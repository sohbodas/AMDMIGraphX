from .base import *
import numpy as np
from transformers import AutoTokenizer


class AutoTokenizerHFMixin(object):

    _processor = None

    @property
    def processor(self):
        if self._processor is None:
            self._processor = AutoTokenizer.from_pretrained(self.model_id)
        return self._processor

    def preprocess(self,
                   question,
                   context,
                   max_length,
                   truncation='only_second'):
        return self.processor(question,
                              context,
                              padding='max_length',
                              max_length=max_length,
                              truncation=truncation,
                              return_tensors="np")


class DistilBERT_base_cased_distilled_SQuAD(SingleOptimumHFModelDownloadMixin,
                                            AutoTokenizerHFMixin, BaseModel):
    @property
    def model_id(self):
        return "distilbert/distilbert-base-cased-distilled-squad"

    @property
    def name(self):
        return "distilbert-base-cased-distilled-squad"


class RobertaBaseSquad2(SingleOptimumHFModelDownloadMixin,
                        AutoTokenizerHFMixin, BaseModel):
    @property
    def model_id(self):
        return "deepset/roberta-base-squad2"

    @property
    def name(self):
        return "roberta-base-squad2"


class GPTJ(SingleOptimumHFModelDownloadMixin, AutoTokenizerHFMixin,
           DecoderModel):
    def __init__(self):
        # no pad token by default
        self.processor.pad_token = self.processor.eos_token

    @property
    def model_id(self):
        return "EleutherAI/gpt-j-6b"

    @property
    def task(self):
        # override to ignore "with-past"
        return "text-generation"

    @property
    def name(self):
        return "gpt-j"

    def preprocess(self, *args, **kwargs):
        # swap squad's default "question - answer" order
        new_args, new_kwargs = list(args), kwargs
        new_args[0], new_args[1] = new_args[1], new_args[0]
        new_kwargs["truncation"] = "only_first"
        result = super().preprocess(*new_args, **new_kwargs)

        # result only contains "input_ids" and "attention_mask", extend it with "position_ids"
        result["position_ids"] = np.arange(0,
                                           len(result["input_ids"][0]),
                                           dtype=np.int64)
        result["position_ids"] = result["position_ids"][np.newaxis]
        return result

    def decode_step(self, input_map, output_map):
        timestep = np.argmax(
            input_map["input_ids"][0] == self.processor.eos_token_id)
        new_token = np.argmax(output_map["logits"][0][timestep - 1])
        input_map["input_ids"][0][timestep] = new_token
        input_map["attention_mask"][0][timestep] = 1
        return new_token == self.processor.eos_token_id
